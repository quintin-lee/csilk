/**
 * @file workflow.c
 * @brief AI Workflow engine implementation with WAL persistence, Tracing,
 * Tools, Monitoring, and Budgeting.
 */

#include "csilk/app/workflow.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "cJSON.h"
#include "csilk/app/workflow_wal.h"
#include "csilk/core/internal.h"
#include "csilk/core/workflow_internal.h"

/** @brief AI metadata attached to workflow node outputs for token
 *  tracking and budget enforcement. */
typedef struct {
	char* model;	       /**< Model name (e.g., "gpt-3.5-turbo"). */
	int prompt_tokens;     /**< Tokens consumed by the prompt. */
	int completion_tokens; /**< Tokens consumed by the completion. */
} csilk_ai_meta_t;

/** @brief A registered workflow tool (function-calling capability exposed
 *  to AI nodes). Tools are invoked in parallel via the libuv thread pool
 *  during AI node execution. */

typedef struct csilk_wf_edge_s {
	char* condition;	 /**< nullptr for default/bind. */
	csilk_wf_node_t* target; /**< Destination node. */
} csilk_wf_edge_t;

/** @brief A single node in a workflow DAG.
 *  Each node wraps a handler function with optional edges, error
 *  handling, dynamic routing, and timeout support. */
struct csilk_wf_node_s {
	char* id;		    /**< Unique string identifier. */
	int index;		    /**< Internal array index for tracking in context. */
	csilk_wf_handler_t handler; /**< Node execution callback. */
	void* user_data;	    /**< Opaque context for the handler. */

	csilk_wf_edge_t* edges; /**< Outgoing edges (conditions leading to next nodes). */
	size_t edge_count;	/**< Number of outgoing edges. */
	size_t edge_capacity;	/**< Allocated edge array capacity. */

	int incoming_count; /**< Number of incoming edges (for join tracking). */
	int is_entry;	    /**< Explicit entry point flag. */

	csilk_wf_node_t* error_target; /**< Fallback node on handler failure (nullptr output). */
	csilk_wf_router_t router_fn;   /**< Dynamic router: overrides edges when set. */
	csilk_wf_join_policy_t join_policy; /**< AND (wait for all) or OR (fire on any). */
	int timeout_ms;			    /**< Per-node execution timeout (0 = no timeout). */
	int is_interactive;		    /**< Requires manual signal to proceed. */
	char* output_schema;		    /**< JSON Schema for output validation. */
	int max_retries;		    /**< Max automatic retry attempts. */
	int retry_delay_ms;		    /**< Delay between retries. */
	int is_remote;			    /**< Offload to remote worker via MQ. */
	void (*user_data_free)(void*);	    /**< Called by node_free to free user_data. */
};

/** @brief Workflow definition: a DAG of processing nodes connected by
 *  conditional or unconditional edges. Each node runs a handler function
 *  on the libuv thread pool. Supports persistence via WAL, real-time
 *  monitoring via WebSocket, tool registration for AI nodes, and budget
 *  (token/TTL) limits. */
struct csilk_wf_s {
	char* name;		 /**< Human-readable workflow name. */
	csilk_wf_node_t** nodes; /**< Array of node pointers. */
	size_t node_count;	 /**< Number of registered nodes. */
	size_t node_capacity;	 /**< Allocated node array capacity. */
	uv_loop_t* loop;	 /**< libuv event loop for thread-pool scheduling. */
	char* wal_dir;		 /**< WAL directory path (nullptr = no persistence). */

	csilk_wf_tool_entry_t* tools; /**< Registered tool definitions. */
	size_t tool_count;	      /**< Number of registered tools. */
	size_t tool_capacity;	      /**< Allocated tool array capacity. */

	csilk_wf_tool_discovery_fn tool_discovery; /**< Dynamic tool discovery callback. */
	void* tool_discovery_user_data;		   /**< User data for discovery callback. */

	csilk_ctx_t** monitors;	  /**< WebSocket monitoring connections. */
	size_t monitor_count;	  /**< Number of active monitors. */
	size_t monitor_capacity;  /**< Allocated monitor array capacity. */
	uv_mutex_t monitor_mutex; /**< Protects the monitor array. */

	int max_tokens; /**< Maximum total tokens across all AI calls (0 = unlimited).
                   */
	int ttl_sec;	/**< Workflow Time-To-Live in seconds (0 = no limit). */
	csilk_mq_t* mq; /**< Optional MQ for distributed execution. */

	csilk_wf_ctx_t** active_contexts;
	size_t active_context_count;
	size_t active_context_capacity;
	uv_mutex_t ctx_mutex;
};

/** @brief Workflow execution context — per-run state tracking all node
 *  progress, scheduling, memory, and budget enforcement. Created by
 *  csilk_wf_run_ext_internal() and freed by cleanup_ctx(). */
struct csilk_wf_ctx_s {
	csilk_wf_t* wf;			 /**< The workflow definition. */
	csilk_data_t* initial_input;	 /**< Original input data passed to run(). */
	void (*callback)(csilk_data_t*); /**< Final result callback. */
	void (*trace_callback)(csilk_data_t*, csilk_wf_trace_t*); /**< Traced result callback. */

	int* node_input_counts; /**< Per-node received-input counters for join
                             tracking. */
	int total_executions;	/**< Total nodes executed (safety counter for infinite
                             loops). */
	int nodes_active;	/**< Nodes currently queued or running on thread pool. */
	uv_mutex_t mutex;	/**< Protects scheduler state (counters, flags). */

	csilk_arena_t* arena;	/**< Memory arena for this execution. */
	uv_mutex_t arena_mutex; /**< Protects arena allocations (parallel tool calls). */

	csilk_data_t** node_outputs; /**< Per-node output data history. */

	char exec_id[37]; /**< UUID execution identifier (36 chars + null). */
	char* wal_path;	  /**< Full path to the WAL file for this execution. */

	csilk_wf_trace_t* trace; /**< Execution trace (timing, I/O dumps). */
	uv_mutex_t trace_mutex;	 /**< Protects trace appends from parallel completions. */

	int total_tokens;   /**< Cumulative tokens used across AI nodes. */
	int is_terminated;  /**< Hard stop flag (budget exceeded, TTL expired). */
	int is_paused;	    /**< Workflow is waiting for human input. */
	int* node_approved; /**< Tracking approved interactive nodes. */

	uv_timer_t ttl_timer; /**< Global TTL timer handle. */
	int is_ttl_expired;   /**< TTL expiration flag. */
};

/** @brief Per-node-execution state passed through libuv work requests.
 *  Allocated in execute_node(), freed in after_worker_cb(). */
typedef struct node_work_s {
	uv_work_t req;	       /**< libuv work request (must be first for cast). */
	csilk_wf_ctx_t* ctx;   /**< Workflow execution context. */
	csilk_wf_node_t* node; /**< The node being executed. */
	csilk_data_t* input;   /**< Input data to the node's handler. */
	csilk_data_t* output;  /**< Output data from the handler (set by worker_cb). */
	csilk_wf_trace_node_t*
	    trace_node;	       /**< Trace record for this node (nullptr if not tracing). */
	uv_timer_t node_timer; /**< Per-node timeout or retry delay timer. */
	int is_timed_out;      /**< Flag set by timer if node exceeds timeout_ms. */
	int retry_count;       /**< Current retry attempt. */
	int timer_closing;     /**< Non-zero once uv_close is called on node_timer. */
} node_work_t;

/* --- Internal Helpers --- */
static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);
static void cleanup_ctx(csilk_wf_ctx_t* ctx);
static void after_worker_cb(uv_work_t* req, int status);

static void register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx);
static void unregister_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx);
static csilk_wf_ctx_t* find_active_ctx(csilk_wf_t* wf, const char* exec_id);

static const char* csilk_wf_run_ext_internal(csilk_wf_t* wf,
					     csilk_data_t* input,
					     void (*callback)(csilk_data_t*),
					     void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*));

/* --- Lifecycle --- */

#include <sys/stat.h>
#include <unistd.h>

static void
serve_ui_handler(csilk_ctx_t* c)
{
	const char* paths[] = {"share/csilk/workflow_ui.html",
			       "../share/csilk/workflow_ui.html",
			       "/usr/local/share/csilk/workflow_ui.html"};

	for (int i = 0; i < 3; i++) {
		FILE* f = fopen(paths[i], "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char* buf = malloc(sz + 1);
			if (buf) {
				if (fread(buf, 1, sz, f) == (size_t)sz) {
					buf[sz] = '\0';
					csilk_set_header(c, "Content-Type", "text/html");
					csilk_string(c, 200, buf);
				}
				free(buf);
			}
			fclose(f);
			return;
		}
	}
	csilk_string(c, 404, "Workflow UI template not found.");
}

void
csilk_wf_serve_ui(csilk_app_t* app, const char* path)
{
	if (!app || !path) {
		return;
	}
	csilk_app_get(app, path, serve_ui_handler);
}

csilk_wf_t*
csilk_wf_new(const char* name)
{
	csilk_wf_t* wf = calloc(1, sizeof(csilk_wf_t));
	if (wf) {
		wf->name = strdup(name);
		wf->loop = uv_default_loop();
		uv_mutex_init(&wf->monitor_mutex);
		uv_mutex_init(&wf->ctx_mutex);
	}
	return wf;
}

static void
ai_config_free(void* ptr)
{
	csilk_ai_config_t* cfg = (csilk_ai_config_t*)ptr;
	if (!cfg) {
		return;
	}
	free((void*)cfg->model);
	free((void*)cfg->system_msg);
	free((void*)cfg->prompt);
	free(cfg);
}

/**
 * @brief Free a vector search configuration.
 */
static void
vector_search_config_free(void* ptr)
{
	csilk_vector_search_config_t* cfg = (csilk_vector_search_config_t*)ptr;
	if (!cfg) {
		return;
	}
	free((void*)cfg->embedding_model);
	free((void*)cfg->collection);
	free((void*)cfg->input_template);
	free(cfg);
}

static void
node_free(csilk_wf_node_t* node)
{
	if (!node) {
		return;
	}
	free(node->id);
	free(node->output_schema);
	for (size_t i = 0; i < node->edge_count; i++) {
		free(node->edges[i].condition);
	}
	free(node->edges);
	if (node->user_data_free) {
		node->user_data_free(node->user_data);
	}
	free(node);
}

void
csilk_wf_free(csilk_wf_t* wf)
{
	if (!wf) {
		return;
	}
	free(wf->name);
	free(wf->wal_dir);
	for (size_t i = 0; i < wf->node_count; i++) {
		node_free(wf->nodes[i]);
	}
	free(wf->nodes);
	for (size_t i = 0; i < wf->tool_count; i++) {
		free(wf->tools[i].name);
		free(wf->tools[i].description);
		free(wf->tools[i].parameters_json);
	}
	free(wf->tools);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		cleanup_ctx(wf->active_contexts[i]);
	}
	free(wf->active_contexts);
	uv_mutex_destroy(&wf->monitor_mutex);
	uv_mutex_destroy(&wf->ctx_mutex);
	free(wf->monitors);
	free(wf);
}

csilk_wf_node_t*
csilk_wf_add(csilk_wf_t* wf, const char* id, csilk_wf_handler_t handler, void* user_data)
{
	if (!wf || !id) {
		return nullptr;
	}
	if (wf->node_count >= wf->node_capacity) {
		size_t new_cap = wf->node_capacity == 0 ? 8 : wf->node_capacity * 2;
		csilk_wf_node_t** new_nodes =
		    realloc(wf->nodes, sizeof(csilk_wf_node_t*) * new_cap);
		if (!new_nodes) {
			return nullptr;
		}
		wf->nodes = new_nodes;
		wf->node_capacity = new_cap;
	}
	csilk_wf_node_t* node = calloc(1, sizeof(csilk_wf_node_t));
	if (!node) {
		return nullptr;
	}
	node->id = strdup(id);
	node->index = (int)wf->node_count;
	node->handler = handler;
	node->user_data = user_data;
	node->join_policy = CSILK_WF_JOIN_AND;
	wf->nodes[wf->node_count++] = node;
	return node;
}

void
csilk_wf_node_set_entry(csilk_wf_node_t* node, int is_entry)
{
	if (node) {
		node->is_entry = is_entry;
	}
}

/* --- Connections --- */

static void
node_add_edge(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to, int is_loop)
{
	if (!from || !to) {
		return;
	}
	if (from->edge_count >= from->edge_capacity) {
		size_t new_cap = from->edge_capacity == 0 ? 4 : from->edge_capacity * 2;
		csilk_wf_edge_t* new_edges =
		    realloc(from->edges, sizeof(csilk_wf_edge_t) * new_cap);
		if (!new_edges) {
			return;
		}
		from->edges = new_edges;
		from->edge_capacity = new_cap;
	}
	from->edges[from->edge_count].condition = condition ? strdup(condition) : nullptr;
	from->edges[from->edge_count].target = to;
	from->edge_count++;
	if (!is_loop) {
		to->incoming_count++;
	}
}

void
csilk_wf_bind(csilk_wf_node_t* from, csilk_wf_node_t* to)
{
	node_add_edge(from, nullptr, to, 0);
}
void
csilk_wf_on(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to)
{
	node_add_edge(from, condition, to, 0);
}
void
csilk_wf_on_loop(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to)
{
	node_add_edge(from, condition, to, 1);
}
void
csilk_wf_on_error(csilk_wf_node_t* from, csilk_wf_node_t* target)
{
	if (from) {
		from->error_target = target;
	}
}
void
csilk_wf_route(csilk_wf_node_t* node, csilk_wf_router_t router)
{
	if (node) {
		node->router_fn = router;
	}
}
void
csilk_wf_node_set_join(csilk_wf_node_t* node, csilk_wf_join_policy_t policy)
{
	if (node) {
		node->join_policy = policy;
	}
}

/* --- Persistence --- */

void
csilk_wf_set_persistence(csilk_wf_t* wf, const char* wal_dir)
{
	if (!wf) {
		return;
	}
	free(wf->wal_dir);
	wf->wal_dir = wal_dir ? strdup(wal_dir) : nullptr;
}

/* --- Monitoring --- */

void
csilk_wf_register_monitor(csilk_wf_t* wf, csilk_ctx_t* c)
{
	if (!wf || !c) {
		return;
	}
	uv_mutex_lock(&wf->monitor_mutex);
	if (wf->monitor_count >= wf->monitor_capacity) {
		size_t new_cap = wf->monitor_capacity == 0 ? 4 : wf->monitor_capacity * 2;
		csilk_ctx_t** new_monitors = realloc(wf->monitors, sizeof(csilk_ctx_t*) * new_cap);
		if (new_monitors) {
			wf->monitors = new_monitors;
			wf->monitor_capacity = new_cap;
		}
	}
	if (wf->monitor_count < wf->monitor_capacity) {
		wf->monitors[wf->monitor_count++] = c;
	}
	uv_mutex_unlock(&wf->monitor_mutex);
}

static void
_wf_broadcast(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload)
{
	if (!wf || wf->monitor_count == 0) {
		return;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "event", event);
	if (node_id) {
		cJSON_AddStringToObject(root, "node_id", node_id);
	}
	if (payload) {
		cJSON_AddStringToObject(root, "payload", payload);
	}
	cJSON_AddNumberToObject(root, "timestamp", (double)time(nullptr));
	char* json = cJSON_PrintUnformatted(root);
	uv_mutex_lock(&wf->monitor_mutex);
	for (size_t i = 0; i < wf->monitor_count; i++) {
		csilk_ws_send(wf->monitors[i], (uint8_t*)json, strlen(json), 0x1);
	}
	uv_mutex_unlock(&wf->monitor_mutex);
	free(json);
	cJSON_Delete(root);
}

/* --- Budget --- */

void
csilk_wf_set_budget(csilk_wf_t* wf, int max_tokens)
{
	if (wf) {
		wf->max_tokens = max_tokens;
	}
}

void
csilk_wf_node_set_timeout(csilk_wf_node_t* node, int timeout_ms)
{
	if (node) {
		node->timeout_ms = timeout_ms;
	}
}

void
csilk_wf_set_ttl(csilk_wf_t* wf, int ttl_sec)
{
	if (wf) {
		wf->ttl_sec = ttl_sec;
	}
}

void
csilk_wf_node_set_interactive(csilk_wf_node_t* node, int is_interactive)
{
	if (node) {
		node->is_interactive = is_interactive;
	}
}

void
csilk_wf_node_set_schema(csilk_wf_node_t* node, const char* schema)
{
	if (!node) {
		return;
	}
	free(node->output_schema);
	node->output_schema = schema ? strdup(schema) : nullptr;
}

void
csilk_wf_node_set_retry(csilk_wf_node_t* node, int max_retries, int retry_delay_ms)
{
	if (!node) {
		return;
	}
	node->max_retries = max_retries;
	node->retry_delay_ms = retry_delay_ms;
}

static csilk_data_t*
remote_pass_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	return input;
}

void
csilk_wf_node_set_remote(csilk_wf_node_t* node, int is_remote)
{
	if (!node) {
		return;
	}
	node->is_remote = is_remote;
	if (is_remote && node->handler == nullptr) {
		node->handler = remote_pass_handler;
	}
}

/* --- Registry for Distributed Workflows --- */
static csilk_wf_t* g_distributed_wfs[32];
static size_t g_distributed_wf_count = 0;

static void
on_remote_result(csilk_mq_ctx_t* m_ctx)
{
	size_t len;
	const char* payload = csilk_mq_get_payload(m_ctx, &len);
	if (!payload) {
		csilk_mq_next(m_ctx);
		return;
	}
	printf("[Workflow] Received remote result: %s\n", payload);

	cJSON* root = cJSON_Parse(payload);
	if (!root) {
		csilk_mq_next(m_ctx);
		return;
	}

	cJSON* j_exec_id = cJSON_GetObjectItem(root, "exec_id");
	cJSON* j_node_id = cJSON_GetObjectItem(root, "node_id");
	cJSON* j_output = cJSON_GetObjectItem(root, "output");

	if (j_exec_id && cJSON_IsString(j_exec_id) && j_output && cJSON_IsString(j_output)) {
		const char* exec_id = j_exec_id->valuestring;
		const char* node_id = j_node_id ? j_node_id->valuestring : nullptr;
		const char* output_str = j_output->valuestring;

		for (size_t i = 0; i < g_distributed_wf_count; i++) {
			csilk_wf_t* wf = g_distributed_wfs[i];

			// 1. Check for Active Context (Hot Resume)
			csilk_wf_ctx_t* active = find_active_ctx(wf, exec_id);
			if (active) {
				printf("[Workflow] Found active execution %s, "
				       "resuming hot...\n",
				       exec_id);
				csilk_wf_node_t* n =
				    node_id ? csilk_wf_get_node(wf, node_id) : nullptr;
				// If node_id wasn't provided, we might have to search the context for a
				// paused node

				if (n) {
					uv_mutex_lock(&active->mutex);
					active->node_approved[n->index] = 1;
					active
					    ->nodes_active--; // Decrement the count we added during offload
					uv_mutex_unlock(&active->mutex);

					csilk_data_t* out_data =
					    csilk_wf_data_new(active,
							      "application/json",
							      csilk_wf_strdup(active, output_str));
					execute_node(active, n, out_data);
					break;
				}
			}

			// 2. Fallback to Cold Resume (WAL)
			char path[512];
			snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, exec_id);
			if (access(path, F_OK) == 0) {
				printf("[Workflow] Found WAL for %s, signaling "
				       "continue (cold)...\n",
				       exec_id);
				csilk_data_t out_data = {
				    "application/json", (void*)output_str, nullptr, nullptr};
				csilk_wf_signal_continue(wf, exec_id, &out_data, nullptr);
				break;
			}
		}
	}

	cJSON_Delete(root);
	csilk_mq_next(m_ctx);
}

void
csilk_wf_enable_distributed(csilk_wf_t* wf, csilk_mq_t* mq)
{
	if (!wf || !mq || !wf->wal_dir) {
		return;
	}
	wf->mq = mq;
	if (g_distributed_wf_count < 32) {
		g_distributed_wfs[g_distributed_wf_count++] = wf;
	}
	csilk_mq_subscribe(mq, "csilk.wf.results", on_remote_result);
}

/* --- Memory Helpers --- */
void*
csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size)
{
	if (!ctx) {
		return nullptr;
	}
	uv_mutex_lock(&ctx->arena_mutex);
	void* ptr = csilk_arena_alloc(ctx->arena, size);
	uv_mutex_unlock(&ctx->arena_mutex);
	return ptr;
}

char*
csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s)
{
	if (!s) {
		return nullptr;
	}
	size_t len = strlen(s);
	char* news = csilk_wf_alloc(ctx, len + 1);
	if (news) {
		memcpy(news, s, len + 1);
	}
	return news;
}

csilk_data_t*
csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value)
{
	csilk_data_t* data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
	if (data) {
		data->type = csilk_wf_strdup(ctx, type);
		data->value = value;
		data->free_fn = nullptr;
		data->meta = nullptr;
	}
	return data;
}

/** @brief Internal: traverse a cJSON tree following a dot-separated path.
 *
 * Algorithm:
 * 1. Split the path on "." using strtok_r.
 * 2. For each token, if the current cJSON node is an array, index into
 *    it using atoi(); otherwise, use cJSON_GetObjectItemCaseSensitive().
 * 3. If the final value is a string/number/bool, return it as a string
 *    allocated from the workflow arena. For objects/arrays, return a
 *    stringified JSON representation.
 *
 * @param ctx  Workflow context (for arena allocation).
 * @param root Root cJSON node to start traversal from.
 * @param path Dot-separated path (e.g., "user.address.city").
 * @return A string allocated in ctx->arena, or nullptr if the path does not
 *         exist. The returned string is valid for the workflow's lifetime. */
static char*
_csilk_json_get_path(csilk_wf_ctx_t* ctx, cJSON* root, const char* path)
{
	if (!root || !path) {
		return nullptr;
	}

	cJSON* curr = root;
	char* path_copy = strdup(path);
	char* saveptr;
	char* token = strtok_r(path_copy, ".", &saveptr);

	while (token && curr) {
		if (cJSON_IsArray(curr)) {
			curr = cJSON_GetArrayItem(curr, atoi(token));
		} else {
			curr = cJSON_GetObjectItemCaseSensitive(curr, token);
		}
		token = strtok_r(nullptr, ".", &saveptr);
	}

	char* result = nullptr;
	if (curr) {
		if (cJSON_IsString(curr)) {
			result = csilk_wf_strdup(ctx, curr->valuestring);
		} else if (cJSON_IsNumber(curr)) {
			char buf[64];
			snprintf(buf, sizeof(buf), "%g", curr->valuedouble);
			result = csilk_wf_strdup(ctx, buf);
		} else if (cJSON_IsBool(curr)) {
			result = csilk_wf_strdup(ctx, curr->valueint ? "true" : "false");
		} else if (cJSON_IsNull(curr)) {
			result = csilk_wf_strdup(ctx, "null");
		} else {
			// For objects/arrays, return as stringified JSON
			char* tmp = cJSON_PrintUnformatted(curr);
			result = csilk_wf_strdup(ctx, tmp);
			free(tmp);
		}
	}

	free(path_copy);
	return result;
}

/* --- Template Engine & AI Node --- */

/** @brief Internal: resolve template expressions in a prompt string.
 *
 * Template syntax:
 *   {{node_id.value}}          -> raw value output from a node
 *   {{node_id.value.path.to}}  -> JSONPath into a node's JSON output
 *   {{input.value}}            -> the workflow's initial input
 *   {{input.value.path.to}}    -> JSONPath into the initial input
 *
 * Algorithm:
 * 1. Iterate over all workflow nodes, searching for {{node_id.value}}
 *    patterns in the template string.
 * 2. For each match, look up the node's output. If followed by ".path",
 *    parse the node output as JSON and traverse with _csilk_json_get_path().
 *    Otherwise, use the raw output value.
 * 3. Replace the {{...}} placeholder with the resolved value using arena
 *    memory.
 * 4. Repeat for {{input.value}} patterns using the workflow's initial input.
 *
 * @param ctx      Workflow execution context.
 * @param template Template string with {{...}} placeholders.
 * @return Resolved string allocated in ctx->arena.
 * @note Unresolvable patterns (missing node output, bad path) are replaced
 *       with "(null)". */
static char*
apply_filter(csilk_wf_ctx_t* ctx, const char* filter, char* val)
{
	if (!filter || !val) {
		return val;
	}

	if (strcmp(filter, "upper") == 0) {
		for (int i = 0; val[i]; i++) {
			val[i] = toupper(val[i]);
		}
	} else if (strcmp(filter, "lower") == 0) {
		for (int i = 0; val[i]; i++) {
			val[i] = tolower(val[i]);
		}
	} else if (strcmp(filter, "trim") == 0) {
		char* start = val;
		while (*start && isspace(*start)) {
			start++;
		}
		char* end = val + strlen(val) - 1;
		while (end > start && isspace(*end)) {
			end--;
		}
		*(end + 1) = '\0';
		return start;
	} else if (strncmp(filter, "summarize:", 10) == 0) {
		size_t len = (size_t)atoi(filter + 10);
		if (strlen(val) > len) {
			val[len] = '\0';
		}
	} else if (strcmp(filter, "json_escape") == 0) {
		cJSON* j = cJSON_CreateString(val);
		char* escaped = cJSON_PrintUnformatted(j);
		// Remove surrounding quotes
		size_t elen = strlen(escaped);
		if (elen >= 2) {
			escaped[elen - 1] = '\0';
			char* inner = csilk_wf_strdup(ctx, escaped + 1);
			free(escaped);
			cJSON_Delete(j);
			return inner;
		}
		free(escaped);
		cJSON_Delete(j);
	}
	return val;
}

static char*
resolve_templates(csilk_wf_ctx_t* ctx, const char* template)
{
	if (!template) {
		return nullptr;
	}
	char* res = csilk_wf_strdup(ctx, template);

	// 1. Handle node references: {{node_id.value}} or
	// {{node_id.value.path.to.field}}
	for (size_t i = 0; i < ctx->wf->node_count; i++) {
		csilk_wf_node_t* n = ctx->wf->nodes[i];
		char base_pattern[256];
		snprintf(base_pattern, sizeof(base_pattern), "{{%s.value", n->id);

		char* pos;
		while ((pos = strstr(res, base_pattern)) != nullptr) {
			char* end = strstr(pos, "}}");
			if (!end) {
				break;
			}

			size_t pat_full_len = end - pos + 2;
			char* replacement = "(null)";
			csilk_data_t* out = ctx->node_outputs[n->index];

			if (out && out->value) {
				char* path_start = pos + strlen(base_pattern);
				// We need to find if there is a | before }}
				char* pipe = strchr(path_start, '|');
				if (pipe && pipe > end) {
					pipe = nullptr;
				}

				char* filter_start = pipe ? pipe + 1 : nullptr;
				char* actual_end = pipe ? pipe : end;

				if (*path_start == '.') {
					// JSONPath!
					path_start++; // Skip the dot
					size_t path_len = actual_end - path_start;
					char* path = malloc(path_len + 1);
					memcpy(path, path_start, path_len);
					path[path_len] = '\0';

					cJSON* json = cJSON_Parse((char*)out->value);
					if (json) {
						char* val = _csilk_json_get_path(ctx, json, path);
						if (val) {
							replacement = val;
						}
						cJSON_Delete(json);
					}
					free(path);
				} else {
					// Pure value
					replacement = csilk_wf_strdup(ctx, (char*)out->value);
				}

				// Apply Filters
				if (filter_start) {
					size_t flen = end - filter_start;
					char* filters = malloc(flen + 1);
					memcpy(filters, filter_start, flen);
					filters[flen] = '\0';

					char* saveptr;
					char* f = strtok_r(filters, "|", &saveptr);
					while (f) {
						// Trim filter name
						while (*f == ' ') {
							f++;
						}
						char* fe = f + strlen(f) - 1;
						while (fe > f && *fe == ' ') {
							*fe = '\0';
							fe--;
						}

						replacement = apply_filter(ctx, f, replacement);
						f = strtok_r(nullptr, "|", &saveptr);
					}
					free(filters);
				}
			}

			size_t rep_len = strlen(replacement);
			size_t res_len = strlen(res);
			char* new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
			size_t prefix_len = pos - res;
			memcpy(new_res, res, prefix_len);
			memcpy(new_res + prefix_len, replacement, rep_len);
			memcpy(new_res + prefix_len + rep_len, end + 2, strlen(end + 2) + 1);
			res = new_res;
		}
	}

	// 2. Handle initial input: {{input.value}}
	const char* in_pattern = "{{input.value";
	char* pos;
	while ((pos = strstr(res, in_pattern)) != nullptr) {
		char* end = strstr(pos, "}}");
		if (!end) {
			break;
		}

		size_t pat_full_len = end - pos + 2;
		char* replacement = "(null)";

		if (ctx->initial_input && ctx->initial_input->value) {
			char* path_start = pos + strlen(in_pattern);
			char* pipe = strchr(path_start, '|');
			if (pipe && pipe > end) {
				pipe = nullptr;
			}
			char* filter_start = pipe ? pipe + 1 : nullptr;
			char* actual_end = pipe ? pipe : end;

			if (*path_start == '.') {
				path_start++;
				size_t path_len = actual_end - path_start;
				char* path = malloc(path_len + 1);
				memcpy(path, path_start, path_len);
				path[path_len] = '\0';

				cJSON* json = cJSON_Parse((char*)ctx->initial_input->value);
				if (json) {
					char* val = _csilk_json_get_path(ctx, json, path);
					if (val) {
						replacement = val;
					}
					cJSON_Delete(json);
				}
				free(path);
			} else {
				replacement =
				    csilk_wf_strdup(ctx, (char*)ctx->initial_input->value);
			}

			if (filter_start) {
				size_t flen = end - filter_start;
				char* filters = malloc(flen + 1);
				memcpy(filters, filter_start, flen);
				filters[flen] = '\0';
				char* saveptr;
				char* f = strtok_r(filters, "|", &saveptr);
				while (f) {
					while (*f == ' ') {
						f++;
					}
					char* fe = f + strlen(f) - 1;
					while (fe > f && *fe == ' ') {
						*fe = '\0';
						fe--;
					}
					replacement = apply_filter(ctx, f, replacement);
					f = strtok_r(nullptr, "|", &saveptr);
				}
				free(filters);
			}
		}

		size_t rep_len = strlen(replacement);
		size_t res_len = strlen(res);
		char* new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
		size_t prefix_len = pos - res;
		memcpy(new_res, res, prefix_len);
		memcpy(new_res + prefix_len, replacement, rep_len);
		memcpy(new_res + prefix_len + rep_len, end + 2, strlen(end + 2) + 1);
		res = new_res;
	}

	return res;
}

/** @brief Per-tool-call context for parallel tool execution within an
 *  AI node. Each tool call runs on its own libuv thread-pool worker. */
typedef struct {
	csilk_wf_ctx_t* ctx;		   /**< Workflow context (for tool registry lookup). */
	csilk_ai_tool_call_t* tc;	   /**< Tool call arguments from the AI response. */
	char* result;			   /**< Tool output string (allocated by tool fn). */
	uv_mutex_t* mutex;		   /**< Shared mutex for the pending counter. */
	uv_cond_t* cond;		   /**< Shared condition variable for completion. */
	int* pending;			   /**< Shared atomic-like pending count. */
	csilk_wf_tool_entry_t* discovered; /**< Dynamically discovered tools. */
	size_t discovered_count;	   /**< Number of discovered tools. */
} sub_tool_work_t;

/** @brief libuv thread-pool work callback for tool execution.
 *  Looks up the tool by name in the workflow's tool registry and
 *  calls its function with the arguments from the AI tool call. */
static void
sub_worker_cb(uv_work_t* req)
{
	sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
	sw->result = nullptr;
	for (size_t j = 0; j < sw->ctx->wf->tool_count; j++) {
		if (strcmp(sw->ctx->wf->tools[j].name, sw->tc->name) == 0) {
			sw->result = sw->ctx->wf->tools[j].fn(sw->tc->arguments,
							      sw->ctx->wf->tools[j].user_data);
			return;
		}
	}
	for (size_t j = 0; j < sw->discovered_count; j++) {
		if (strcmp(sw->discovered[j].name, sw->tc->name) == 0) {
			sw->result =
			    sw->discovered[j].fn(sw->tc->arguments, sw->discovered[j].user_data);
			return;
		}
	}
}

/** @brief libuv after-work callback for tool execution.
 *  Decrements the shared pending counter and signals the condition
 *  variable to wake the main AI node handler thread. */
static void
after_sub_worker_cb(uv_work_t* req, int status)
{
	(void)status;
	sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
	uv_mutex_lock(sw->mutex);
	(*sw->pending)--;
	uv_cond_signal(sw->cond);
	uv_mutex_unlock(sw->mutex);
}

typedef struct {
	csilk_wf_ctx_t* ctx;
	const char* node_id;
} stream_ctx_t;

static void
on_ai_stream(const char* chunk, void* user_data)
{
	stream_ctx_t* s_ctx = (stream_ctx_t*)user_data;
	_wf_broadcast(s_ctx->ctx->wf, "node_stream", s_ctx->node_id, chunk);
}

/** @brief Built-in handler for AI workflow nodes.
 *
 * Algorithm:
 * 1. Resolve template expressions in the prompt string.
 * 2. Create an AI engine instance (OpenAI driver by default) using
 *    AGENT_API_KEY and AGENT_API_BASE environment variables.
 * 3. Build tool definitions from the workflow's registered tools.
 * 4. Construct a message array (optional system message + user prompt).
 * 5. Enter a request loop (max 10 iterations):
 *    a. Send a chat completion request with the current message array.
 *    b. If the response contains tool calls, execute each tool in
 *       parallel on the libuv thread pool, wait for all to complete
 *       via a condition variable, append tool results as new messages.
 *    c. If the response is a direct text completion, extract the content
 *       and AI metadata (model, token counts), return as csilk_data_t.
 * 6. Clean up all temporary allocations (messages, tools, AI handle).
 *
 * @param ctx       Workflow execution context.
 * @param input     Ignored (AI prompts come from config templates).
 * @param user_data Pointer to csilk_ai_config_t with model, prompt, etc.
 * @return Output data with type "text/plain" and AI metadata, or nullptr on
 *         failure (missing API key, driver init failure, all retries
 *         exhausted). */
static csilk_data_t*
ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	csilk_ai_config_t* config = (csilk_ai_config_t*)user_data;
	char* prompt = resolve_templates(ctx, config->prompt);
	const char* api_key = getenv("AGENT_API_KEY");
	if (!api_key) {
		return nullptr;
	}
	csilk_ai_t* ai = csilk_ai_new("openai", api_key, getenv("AGENT_API_BASE"));
	if (!ai) {
		return nullptr;
	}
	csilk_ai_tool_t* tools = nullptr;
	csilk_wf_tool_entry_t* discovered = nullptr;
	size_t discovered_count = 0;

	/* Call dynamic discovery callback if set */
	if (ctx->wf->tool_discovery) {
		ctx->wf->tool_discovery(ctx->wf,
					ctx->wf->tools,
					ctx->wf->tool_count,
					&discovered,
					&discovered_count,
					ctx->wf->tool_discovery_user_data);
	}

	size_t total_tools = ctx->wf->tool_count + discovered_count;
	if (total_tools > 0) {
		tools = calloc(total_tools, sizeof(csilk_ai_tool_t));
		/* Static tools first (take precedence on name collision) */
		for (size_t i = 0; i < ctx->wf->tool_count; i++) {
			tools[i].type = "function";
			tools[i].function.name = ctx->wf->tools[i].name;
			tools[i].function.description = ctx->wf->tools[i].description;
			if (ctx->wf->tools[i].parameters_json) {
				tools[i].function.parameters_json =
				    cJSON_Parse(ctx->wf->tools[i].parameters_json);
			}
		}
		/* Discovered tools appended after static ones */
		for (size_t i = 0; i < discovered_count; i++) {
			size_t idx = ctx->wf->tool_count + i;
			tools[idx].type = "function";
			tools[idx].function.name = discovered[i].name;
			tools[idx].function.description = discovered[i].description;
			if (discovered[i].parameters_json) {
				tools[idx].function.parameters_json =
				    cJSON_Parse(discovered[i].parameters_json);
			}
		}
	}

	stream_ctx_t s_ctx = {ctx, "unknown"};
	for (size_t i = 0; i < ctx->wf->node_count; i++) {
		if (ctx->wf->nodes[i]->handler == ai_node_handler &&
		    ctx->wf->nodes[i]->user_data == user_data) {
			s_ctx.node_id = ctx->wf->nodes[i]->id;
			break;
		}
	}

	size_t msg_capacity = 32;
	csilk_ai_message_t* msgs = calloc(msg_capacity, sizeof(csilk_ai_message_t));
	size_t msg_count = 0;
	if (config->system_msg) {
		msgs[msg_count].role = "system";
		msgs[msg_count].content = strdup(config->system_msg);
		msg_count++;
	}
	msgs[msg_count].role = "user";
	msgs[msg_count].content = strdup(prompt);
	msg_count++;
	csilk_data_t* out = nullptr;
	int iterations = 0;
	while (iterations < 10) {
		iterations++;

		// Enforce context window limit
		if (config->max_history_messages > 0 &&
		    msg_count > (size_t)config->max_history_messages) {
			size_t keep = (size_t)config->max_history_messages;
			size_t discard_start = 0;
			size_t move_to = 0;

			// If msgs[0] is system, always keep it
			if (strcmp(msgs[0].role, "system") == 0) {
				discard_start = 1;
				move_to = 1;
				keep--;
			}

			size_t discard_count = msg_count - (size_t)config->max_history_messages;
			// Free discarded messages
			for (size_t i = discard_start; i < discard_start + discard_count; i++) {
				free((void*)msgs[i].content);
			}

			// Shift remaining messages
			memmove(&msgs[move_to],
				&msgs[discard_start + discard_count],
				sizeof(csilk_ai_message_t) * keep);
			msg_count = (size_t)config->max_history_messages;
		}

		csilk_ai_chat_request_t req = {
		    .model = config->model ? config->model : "gpt-3.5-turbo",
		    .messages = msgs,
		    .message_count = msg_count,
		    .temperature = config->temperature > 0 ? config->temperature : 0.7,
		    .max_tokens = config->max_tokens > 0 ? config->max_tokens : 1024,
		    .tools = tools,
		    .tool_count = ctx->wf->tool_count,
		    .on_chunk = config->stream ? on_ai_stream : nullptr,
		    .user_data = config->stream ? &s_ctx : nullptr};
		csilk_ai_chat_response_t res;
		if (csilk_ai_chat(ai, &req, &res) != 0) {
			break;
		}
		if (res.tool_call_count > 0) {
			if (msg_count + res.tool_call_count + 1 >= msg_capacity) {
				msg_capacity += res.tool_call_count + 16;
				msgs = realloc(msgs, sizeof(csilk_ai_message_t) * msg_capacity);
			}
			msgs[msg_count].role = "assistant";
			msgs[msg_count].content = res.content ? strdup(res.content) : strdup("");
			msg_count++;

			// Parallel Execution of Tool Calls
			uv_mutex_t m;
			uv_cond_t c;
			int pending = (int)res.tool_call_count;
			uv_mutex_init(&m);
			uv_cond_init(&c);
			sub_tool_work_t* sws = calloc(res.tool_call_count, sizeof(sub_tool_work_t));
			uv_work_t* reqs = calloc(res.tool_call_count, sizeof(uv_work_t));

			for (size_t i = 0; i < res.tool_call_count; i++) {
				sws[i].ctx = ctx;
				sws[i].tc = &res.tool_calls[i];
				sws[i].mutex = &m;
				sws[i].cond = &c;
				sws[i].pending = &pending;
				sws[i].discovered = discovered;
				sws[i].discovered_count = discovered_count;
				reqs[i].data = &sws[i];
				uv_queue_work(
				    ctx->wf->loop, &reqs[i], sub_worker_cb, after_sub_worker_cb);
			}

			uv_mutex_lock(&m);
			while (pending > 0) {
				uv_cond_wait(&c, &m);
			}
			uv_mutex_unlock(&m);

			for (size_t i = 0; i < res.tool_call_count; i++) {
				msgs[msg_count].role = "tool";
				msgs[msg_count].content =
				    sws[i].result ? sws[i].result : strdup("{}");
				msg_count++;
			}

			free(sws);
			free(reqs);
			uv_mutex_destroy(&m);
			uv_cond_destroy(&c);
			csilk_ai_chat_response_free(&res);
			continue;
		}
		out = csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, res.content));
		csilk_ai_meta_t* meta = csilk_wf_alloc(ctx, sizeof(csilk_ai_meta_t));
		meta->model = csilk_wf_strdup(ctx, req.model);
		meta->prompt_tokens = res.prompt_tokens;
		meta->completion_tokens = res.completion_tokens;
		out->meta = meta;
		csilk_ai_chat_response_free(&res);
		break;
	}
	for (size_t i = 0; i < msg_count; i++) {
		free((void*)msgs[i].content);
	}
	free(msgs);
	if (tools) {
		for (size_t i = 0; i < ctx->wf->tool_count; i++) {
			cJSON_Delete(tools[i].function.parameters_json);
		}
		free(tools);
	}
	csilk_ai_free(ai);
	return out;
}

/**
 * @brief Internal built-in handler for Vector Search nodes.
 */
static csilk_data_t*
vector_search_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	csilk_vector_search_config_t* config = (csilk_vector_search_config_t*)user_data;
	if (!config || !config->ai || !config->db || !config->collection) {
		return nullptr;
	}

	/* 1. Get input text (either from template or input value) */
	const char* text = nullptr;
	char* resolved = nullptr;
	if (config->input_template) {
		resolved = resolve_templates(ctx, config->input_template);
		text = resolved;
	} else if (input && input->value && strcmp(input->type, "text/plain") == 0) {
		text = (const char*)input->value;
	}

	if (!text) {
		return nullptr;
	}

	/* 2. Generate embeddings */
	csilk_ai_embeddings_response_t eres = {0};
	if (csilk_ai_embeddings(config->ai, config->embedding_model, &text, 1, &eres) != 0) {
		csilk_ai_embeddings_response_free(&eres);
		return nullptr;
	}

	if (eres.count == 0 || eres.dimension == 0) {
		csilk_ai_embeddings_response_free(&eres);
		return nullptr;
	}

	/* 3. Search Vector DB */
	csilk_vector_search_response_t vres = {0};
	if (csilk_vector_db_search(config->db,
				   config->collection,
				   eres.values,
				   eres.dimension,
				   config->limit > 0 ? config->limit : 5,
				   &vres) != 0) {
		csilk_ai_embeddings_response_free(&eres);
		csilk_vector_search_response_free(&vres);
		return nullptr;
	}

	/* 4. Convert results to JSON */
	cJSON* root = cJSON_CreateArray();
	for (size_t i = 0; i < vres.count; i++) {
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "id", vres.results[i].id);
		cJSON_AddNumberToObject(item, "score", (double)vres.results[i].score);
		if (vres.results[i].payload) {
			cJSON_AddItemToObject(
			    item, "payload", cJSON_Duplicate(vres.results[i].payload, 1));
		}
		cJSON_AddItemToArray(root, item);
	}

	char* json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	/* 5. Cleanup and return */
	csilk_ai_embeddings_response_free(&eres);
	csilk_vector_search_response_free(&vres);

	csilk_data_t* out = csilk_wf_data_new(ctx, "application/json", json_str);
	if (out) {
		out->free_fn = free;
	} else {
		free(json_str);
	}
	return out;
}

csilk_wf_node_t*
csilk_wf_add_ai(csilk_wf_t* wf, const char* id, const csilk_ai_config_t* config)
{
	csilk_ai_config_t* copy = malloc(sizeof(csilk_ai_config_t));
	memcpy(copy, config, sizeof(csilk_ai_config_t));
	copy->model = config->model ? strdup(config->model) : nullptr;
	copy->system_msg = config->system_msg ? strdup(config->system_msg) : nullptr;
	copy->prompt = config->prompt ? strdup(config->prompt) : nullptr;
	csilk_wf_node_t* node = csilk_wf_add(wf, id, ai_node_handler, copy);
	if (node) {
		node->user_data_free = ai_config_free;
	}
	return node;
}

csilk_wf_node_t*
csilk_wf_add_vector_search(csilk_wf_t* wf,
			   const char* id,
			   const csilk_vector_search_config_t* config)
{
	csilk_vector_search_config_t* copy = malloc(sizeof(csilk_vector_search_config_t));
	memcpy(copy, config, sizeof(csilk_vector_search_config_t));
	copy->embedding_model = config->embedding_model ? strdup(config->embedding_model) : nullptr;
	copy->collection = config->collection ? strdup(config->collection) : nullptr;
	copy->input_template = config->input_template ? strdup(config->input_template) : nullptr;
	/* AI and DB handles are shared, not copied or freed by the node */

	csilk_wf_node_t* node = csilk_wf_add(wf, id, vector_search_node_handler, copy);
	if (node) {
		node->user_data_free = vector_search_config_free;
	}
	return node;
}

void
csilk_wf_register_tool(csilk_wf_t* wf,
		       const char* name,
		       const char* description,
		       const char* parameters_json,
		       csilk_wf_tool_fn fn,
		       void* user_data)
{
	if (!wf || !name || !fn) {
		return;
	}
	uv_mutex_lock(&wf->monitor_mutex);
	if (wf->tool_count >= wf->tool_capacity) {
		size_t new_cap = wf->tool_capacity == 0 ? 4 : wf->tool_capacity * 2;
		csilk_wf_tool_entry_t* new_tools =
		    realloc(wf->tools, sizeof(csilk_wf_tool_entry_t) * new_cap);
		if (new_tools) {
			wf->tools = new_tools;
			wf->tool_capacity = new_cap;
		}
	}
	if (wf->tool_count < wf->tool_capacity) {
		csilk_wf_tool_entry_t* entry = &wf->tools[wf->tool_count++];
		entry->name = strdup(name);
		entry->description = description ? strdup(description) : nullptr;
		entry->parameters_json = parameters_json ? strdup(parameters_json) : nullptr;
		entry->fn = fn;
		entry->user_data = user_data;
	}
	uv_mutex_unlock(&wf->monitor_mutex);
}

/**
 * @brief Set a dynamic tool discovery callback for the workflow.
 * @see csilk_wf_tool_discovery_fn in workflow.h for callback contract.
 */
void
csilk_wf_set_tool_discovery(csilk_wf_t* wf, csilk_wf_tool_discovery_fn discovery, void* user_data)
{
	if (!wf) {
		return;
	}
	uv_mutex_lock(&wf->monitor_mutex);
	wf->tool_discovery = discovery;
	wf->tool_discovery_user_data = user_data;
	uv_mutex_unlock(&wf->monitor_mutex);
}

/** @brief Look up a workflow node by its string ID.
 *  @param wf The workflow instance.
 *  @param id Node identifier (set in csilk_wf_add()).
 *  @return Node pointer, or nullptr if not found.
 *  @note Linear search of the node array — O(n). */
csilk_wf_node_t*
csilk_wf_get_node(csilk_wf_t* wf, const char* id)
{
	if (!wf || !id) {
		return nullptr;
	}
	for (size_t i = 0; i < wf->node_count; i++) {
		if (strcmp(wf->nodes[i]->id, id) == 0) {
			return wf->nodes[i];
		}
	}
	return nullptr;
}

/* --- Visualization --- */

char*
csilk_wf_to_mermaid(csilk_wf_t* wf)
{
	if (!wf) {
		return nullptr;
	}
	size_t buf_size = 8192;
	char* buf = malloc(buf_size);
	size_t buf_used = 0;
	buf_used += (size_t)snprintf(buf, buf_size, "graph TD\n");
	for (size_t i = 0; i < wf->node_count; i++) {
		csilk_wf_node_t* n = wf->nodes[i];
		char line[512];
		snprintf(line, sizeof(line), "  \"%s\"[\"%s\"]\n", n->id, n->id);
		if (buf_used + strlen(line) < buf_size) {
			buf_used +=
			    (size_t)snprintf(buf + buf_used, buf_size - buf_used, "%s", line);
		}
		for (size_t j = 0; j < n->edge_count; j++) {
			csilk_wf_edge_t* e = &n->edges[j];
			if (e->condition) {
				snprintf(line,
					 sizeof(line),
					 "  \"%s\" -- \"%s\" --> \"%s\"\n",
					 n->id,
					 e->condition,
					 e->target->id);
			} else {
				snprintf(line,
					 sizeof(line),
					 "  \"%s\" --> \"%s\"\n",
					 n->id,
					 e->target->id);
			}
			if (buf_used + strlen(line) < buf_size) {
				buf_used += (size_t)snprintf(
				    buf + buf_used, buf_size - buf_used, "%s", line);
			}
		}
		if (n->error_target) {
			snprintf(line,
				 sizeof(line),
				 "  \"%s\" -. \"error\" .-> \"%s\"\n",
				 n->id,
				 n->error_target->id);
			if (buf_used + strlen(line) < buf_size) {
				buf_used += (size_t)snprintf(
				    buf + buf_used, buf_size - buf_used, "%s", line);
			}
		}
	}
	return buf;
}

/* --- Tracing Implementation --- */

char*
csilk_wf_trace_to_json(const csilk_wf_trace_t* trace)
{
	if (!trace) {
		return nullptr;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "exec_id", trace->exec_id);
	cJSON_AddNumberToObject(root, "start_time", (double)trace->start_time);
	cJSON_AddNumberToObject(root, "end_time", (double)trace->end_time);
	cJSON_AddNumberToObject(root, "duration_us", (double)(trace->end_time - trace->start_time));
	cJSON* nodes = cJSON_CreateArray();
	for (size_t i = 0; i < trace->node_count; i++) {
		csilk_wf_trace_node_t* n = trace->nodes[i];
		cJSON* nj = cJSON_CreateObject();
		cJSON_AddStringToObject(nj, "node_id", n->node_id);
		cJSON_AddNumberToObject(nj, "start_time", (double)n->start_time);
		cJSON_AddNumberToObject(nj, "end_time", (double)n->end_time);
		cJSON_AddNumberToObject(nj, "duration_us", (double)(n->end_time - n->start_time));
		if (n->input_dump) {
			cJSON_AddStringToObject(nj, "input", n->input_dump);
		}
		if (n->output_dump) {
			cJSON_AddStringToObject(nj, "output", n->output_dump);
		}
		if (n->model) {
			cJSON_AddStringToObject(nj, "model", n->model);
		}
		if (n->prompt_tokens > 0) {
			cJSON_AddNumberToObject(nj, "prompt_tokens", n->prompt_tokens);
		}
		if (n->completion_tokens > 0) {
			cJSON_AddNumberToObject(nj, "completion_tokens", n->completion_tokens);
		}
		if (n->error) {
			cJSON_AddStringToObject(nj, "error", n->error);
		}
		cJSON_AddItemToArray(nodes, nj);
	}
	cJSON_AddItemToObject(root, "nodes", nodes);
	char* out = cJSON_Print(root);
	cJSON_Delete(root);
	return out;
}

void
csilk_wf_trace_free(csilk_wf_trace_t* trace)
{
	if (!trace) {
		return;
	}
	free(trace->exec_id);
	for (size_t i = 0; i < trace->node_count; i++) {
		csilk_wf_trace_node_t* n = trace->nodes[i];
		free(n->node_id);
		free(n->input_dump);
		free(n->output_dump);
		free(n->model);
		free(n->error);
		free(n);
	}
	free(trace->nodes);
	free(trace);
}

/* --- Scheduler Implementation --- */

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);

static void
register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
	uv_mutex_lock(&wf->ctx_mutex);
	if (wf->active_context_count >= wf->active_context_capacity) {
		size_t new_cap =
		    wf->active_context_capacity == 0 ? 8 : wf->active_context_capacity * 2;
		csilk_wf_ctx_t** new_ctxs =
		    realloc(wf->active_contexts, sizeof(csilk_wf_ctx_t*) * new_cap);
		if (new_ctxs) {
			wf->active_contexts = new_ctxs;
			wf->active_context_capacity = new_cap;
		}
	}
	if (wf->active_context_count < wf->active_context_capacity) {
		wf->active_contexts[wf->active_context_count++] = ctx;
	}
	uv_mutex_unlock(&wf->ctx_mutex);
}

static void
unregister_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
	uv_mutex_lock(&wf->ctx_mutex);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		if (wf->active_contexts[i] == ctx) {
			wf->active_contexts[i] = wf->active_contexts[--wf->active_context_count];
			break;
		}
	}
	uv_mutex_unlock(&wf->ctx_mutex);
}

static csilk_wf_ctx_t*
find_active_ctx(csilk_wf_t* wf, const char* exec_id)
{
	csilk_wf_ctx_t* found = nullptr;
	uv_mutex_lock(&wf->ctx_mutex);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		if (strcmp(wf->active_contexts[i]->exec_id, exec_id) == 0) {
			found = wf->active_contexts[i];
			break;
		}
	}
	uv_mutex_unlock(&wf->ctx_mutex);
	return found;
}

static void
cleanup_ctx_now(csilk_wf_ctx_t* ctx)
{
	uv_mutex_destroy(&ctx->mutex);
	uv_mutex_destroy(&ctx->arena_mutex);
	uv_mutex_destroy(&ctx->trace_mutex);
	csilk_arena_free(ctx->arena);
	free(ctx->node_input_counts);
	free(ctx->node_approved);
	free(ctx->node_outputs);
	free(ctx->wal_path);
	free(ctx);
}

static void
on_ttl_timer_close(uv_handle_t* handle)
{
	csilk_wf_ctx_t* ctx = (csilk_wf_ctx_t*)handle->data;
	cleanup_ctx_now(ctx);
}

/** @brief Internal: free a workflow execution context and all resources.
 *
 * Stops and closes the TTL timer if active, destroys all mutexes,
 * frees the memory arena, node tracking arrays, WAL path, and the
 * context struct itself.
 *
 * @param ctx The execution context to clean up (may be nullptr). */
static void
cleanup_ctx(csilk_wf_ctx_t* ctx)
{
	if (!ctx) {
		return;
	}
	unregister_active_ctx(ctx->wf, ctx);
	if (ctx->wf->ttl_sec > 0) {
		uv_timer_stop(&ctx->ttl_timer);
		if (!uv_is_closing((uv_handle_t*)&ctx->ttl_timer)) {
			ctx->ttl_timer.data = ctx;
			uv_close((uv_handle_t*)&ctx->ttl_timer, on_ttl_timer_close);
			return;
		}
	}
	cleanup_ctx_now(ctx);
}

/** @brief Internal: persist a workflow event to the Write-Ahead Log.
 *
 * Packs node_id, data type, and data value into a flat payload buffer
 * and delegates to _wf_wal_append(). The payload format is:
 *   [node_id\0][data_type\0][data_value\0]
 * Fields are NUL-terminated strings for simple parsing during recovery.
 *
 * @param ctx     Workflow execution context (must have wal_path set).
 * @param type    Event type (WF_EV_START, WF_EV_NODE_START, etc.).
 * @param node_id Originating node ID, or nullptr for workflow-level events.
 * @param data    Associated data (may be nullptr for simple events).
 * @note This is a no-op if ctx has no WAL path configured. */
static void
wal_log_event(csilk_wf_ctx_t* ctx,
	      csilk_wf_event_type_t type,
	      const char* node_id,
	      csilk_data_t* data)
{
	if (!ctx->wal_path) {
		return;
	}

	/* Ensure we always have 3 null-terminated strings for consistency,
     even if some fields are nullptr. This prevents buffer overflows during
     recovery parsing. */
	const char* nid = node_id ? node_id : "";
	const char* d_type = (data && data->type) ? data->type : "";
	const char* d_val = (data && data->value) ? (char*)data->value : "";

	size_t node_id_len = strlen(nid) + 1;
	size_t type_len = strlen(d_type) + 1;
	size_t val_len = strlen(d_val) + 1;

	size_t total_len = node_id_len + type_len + val_len;
	char* payload = malloc(total_len);
	if (!payload) {
		return;
	}

	char* p = payload;
	memcpy(p, nid, node_id_len);
	p += node_id_len;
	memcpy(p, d_type, type_len);
	p += type_len;
	memcpy(p, d_val, val_len);

	_wf_wal_append(ctx->wal_path, type, payload, total_len);
	free(payload);
}

/** @brief libuv thread-pool work callback — executes a workflow node's
 *  handler on a background thread.
 *
 * Broadcasts a "node_start" event to monitors, then calls the node's
 * handler function. The output is stored in the work request struct
 * for retrieval by after_worker_cb on the main loop thread. */
static void
worker_cb(uv_work_t* req)
{
	node_work_t* work = (node_work_t*)req->data;
	_wf_broadcast(work->ctx->wf, "node_start", work->node->id, nullptr);
	work->output = work->node->handler(work->ctx, work->input, work->node->user_data);
}

/** @brief libuv after-work callback — processes node completion on the
 *  main loop thread.
 *
 * Algorithm (the central scheduler dispatch in the workflow engine):
 * 1. Stop the per-node timeout timer if active.
 * 2. Store the output in ctx->node_outputs and accumulate token usage
 *    from AI metadata.
 * 3. Log to WAL (if enabled) and broadcast "node_finish" to monitors.
 * 4. Record trace data (start/end time, input/output dump, model info).
 * 5. Check budget (max_tokens): if exceeded, set is_terminated flag and
 *    terminate the workflow on next idle check.
 * 6. If output is nullptr and error_target is set, route to error node.
 * 7. If the node has a dynamic router function, call it to determine
 *    the next node; otherwise, evaluate each outgoing edge:
 *    - Unconditional edges (condition == nullptr) always match.
 *    - Conditional edges match if output type equals condition string.
 * 8. For matching edges, check the target's join policy: AND join
 *    requires all incoming edges to fire before the target is ready;
 *    OR join fires on any single edge.
 * 9. If no edges are triggered and no nodes are active, the workflow
 *    is complete: log WF_EV_END, deliver the final output via callback,
 *    and clean up the context. */
static void
on_retry_timer(uv_timer_t* handle)
{
	node_work_t* work = (node_work_t*)handle->data;
	printf("[Workflow] Retrying node '%s' (attempt %d/%d)...\n",
	       work->node->id,
	       work->retry_count,
	       work->node->max_retries);
	uv_queue_work(work->ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

static void
on_work_timer_close(uv_handle_t* handle)
{
	node_work_t* work = (node_work_t*)handle->data;
	free(work);
}

static void
free_work(node_work_t* work)
{
	if (work->node->timeout_ms > 0 && !work->timer_closing) {
		work->timer_closing = 1;
		uv_timer_stop(&work->node_timer);
		work->node_timer.data = work;
		uv_close((uv_handle_t*)&work->node_timer, on_work_timer_close);
		return;
	}
	free(work);
}

static void
after_worker_cb(uv_work_t* req, int status)
{
	(void)status;
	node_work_t* work = (node_work_t*)req->data;
	csilk_wf_ctx_t* ctx = work->ctx;
	csilk_wf_node_t* node = work->node;
	csilk_data_t* output = work->output;

	if (work->is_timed_out) {
		output = nullptr;
	}

	// Handle Retries before error logic
	if (output == nullptr && work->retry_count < node->max_retries) {
		work->retry_count++;
		printf("[Workflow] Node '%s' failed, scheduled retry in %dms\n",
		       node->id,
		       node->retry_delay_ms);

		if (node->retry_delay_ms > 0) {
			uv_timer_init(ctx->wf->loop, &work->node_timer);
			work->node_timer.data = work;
			uv_timer_start(&work->node_timer, on_retry_timer, node->retry_delay_ms, 0);
			return; // Wait for timer
		} else {
			uv_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
			return;
		}
	}

	// JSON Schema Validation

	if (output && output->value && node->output_schema) {
		cJSON* schema = cJSON_Parse(node->output_schema);
		cJSON* data = cJSON_Parse((char*)output->value);
		if (schema && data) {
			cJSON* required = cJSON_GetObjectItem(schema, "required");
			if (cJSON_IsArray(required)) {
				for (int i = 0; i < cJSON_GetArraySize(required); i++) {
					cJSON* field = cJSON_GetArrayItem(required, i);
					if (cJSON_IsString(field) &&
					    !cJSON_HasObjectItem(data, field->valuestring)) {
						printf("[Workflow] Node '%s' "
						       "output failed schema: "
						       "missing required "
						       "field '%s'\n",
						       node->id,
						       field->valuestring);
						output = nullptr;
						break;
					}
				}
			}
		} else if (node->output_schema) {
			output = nullptr; // Invalid JSON or Schema
		}
		cJSON_Delete(schema);
		cJSON_Delete(data);
	}

	uv_mutex_lock(&ctx->mutex);

	ctx->node_outputs[node->index] = output;
	if (output && output->meta) {
		csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
		ctx->total_tokens += am->prompt_tokens + am->completion_tokens;
	}
	uv_mutex_unlock(&ctx->mutex);

	wal_log_event(ctx, WF_EV_NODE_FINISH, node->id, output);
	_wf_broadcast(ctx->wf, "node_finish", node->id, output ? (char*)output->value : nullptr);

	if (work->trace_node) {
		work->trace_node->end_time = uv_hrtime() / 1000;
		if (output) {
			work->trace_node->output_dump =
			    strdup(output->value ? (char*)output->value : "(null)");
			if (output->meta) {
				csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
				work->trace_node->model = strdup(am->model);
				work->trace_node->prompt_tokens = am->prompt_tokens;
				work->trace_node->completion_tokens = am->completion_tokens;
			}
		}
		uv_mutex_lock(&ctx->trace_mutex);
		ctx->trace->nodes =
		    realloc(ctx->trace->nodes,
			    sizeof(csilk_wf_trace_node_t*) * (ctx->trace->node_count + 1));
		ctx->trace->nodes[ctx->trace->node_count++] = work->trace_node;
		uv_mutex_unlock(&ctx->trace_mutex);
	}

	uv_mutex_lock(&ctx->mutex);
	if (ctx->wf->max_tokens > 0 && ctx->total_tokens > ctx->wf->max_tokens) {
		ctx->is_terminated = 1;
		printf("[Workflow] Budget exceeded: %d > %d. Terminating.\n",
		       ctx->total_tokens,
		       ctx->wf->max_tokens);
	}
	int terminated = ctx->is_terminated;
	uv_mutex_unlock(&ctx->mutex);

	if (terminated) {
		uv_mutex_lock(&ctx->mutex);
		ctx->nodes_active--;
		int current_active = ctx->nodes_active;
		uv_mutex_unlock(&ctx->mutex);
		if (current_active == 0) {
			if (ctx->trace_callback) {
				ctx->trace_callback(nullptr, ctx->trace);
			} else if (ctx->callback) {
				ctx->callback(nullptr);
			}
			if (ctx->trace_callback) {
				ctx->trace = nullptr;
			}
			cleanup_ctx(ctx);
		}
		free_work(work);
		return;
	}

	if (output == nullptr && node->error_target) {
		execute_node(ctx, node->error_target, nullptr);
		uv_mutex_lock(&ctx->mutex);
		ctx->nodes_active--;
		uv_mutex_unlock(&ctx->mutex);
		free_work(work);
		return;
	}

	int triggered_count = 0;
	if (node->router_fn) {
		const char* target_id = node->router_fn(output);
		if (target_id) {
			for (size_t i = 0; i < ctx->wf->node_count; i++) {
				if (strcmp(ctx->wf->nodes[i]->id, target_id) == 0) {
					execute_node(ctx, ctx->wf->nodes[i], output);
					triggered_count++;
					break;
				}
			}
		}
	} else {
		for (size_t i = 0; i < node->edge_count; i++) {
			csilk_wf_edge_t* edge = &node->edges[i];
			int match = 0;
			if (edge->condition == nullptr) {
				match = 1;
			} else if (output && output->type &&
				   strcmp(output->type, edge->condition) == 0) {
				match = 1;
			}

			printf("[Workflow] Evaluating edge %zu to '%s' "
			       "(match=%d)\n",
			       i,
			       edge->target->id,
			       match);

			if (match) {
				csilk_wf_node_t* target = edge->target;
				int ready = 0;
				uv_mutex_lock(&ctx->mutex);
				if (ctx->total_executions < MAX_WORKFLOW_STEPS) {
					ctx->node_input_counts[target->index]++;
					int threshold = target->incoming_count == 0
							    ? 1
							    : target->incoming_count;

					printf("[Workflow] Node '%s' input count: "
					       "%d/%d\n",
					       target->id,
					       ctx->node_input_counts[target->index],
					       threshold);

					if (ctx->node_input_counts[target->index] >= threshold) {
						ready = 1;
						ctx->node_input_counts[target->index] = 0;
					}
				}
				uv_mutex_unlock(&ctx->mutex);
				if (ready) {
					printf("[Workflow] Triggering node '%s'\n", target->id);
					execute_node(ctx, target, output);
					triggered_count++;
				}
			}
		}
	}

	uv_mutex_lock(&ctx->mutex);
	ctx->nodes_active--;
	int current_active = ctx->nodes_active;
	uv_mutex_unlock(&ctx->mutex);
	if (triggered_count == 0 && current_active == 0) {
		wal_log_event(ctx, WF_EV_END, nullptr, nullptr);
		_wf_broadcast(
		    ctx->wf, "workflow_end", nullptr, output ? (char*)output->value : nullptr);
		if (ctx->trace) {
			ctx->trace->end_time = uv_hrtime() / 1000;
		}
		if (ctx->trace_callback) {
			ctx->trace_callback(output, ctx->trace);
		} else if (ctx->callback) {
			ctx->callback(output);
		}
		if (ctx->trace_callback) {
			ctx->trace = nullptr;
		} else if (ctx->trace) {
			csilk_wf_trace_free(ctx->trace);
			ctx->trace = nullptr;
		}
		cleanup_ctx(ctx);
	}
	free_work(work);
}

/** @brief libuv timer callback — marks a node as timed out.
 *  Sets the is_timed_out flag on the node_work_t, which causes
 *  after_worker_cb to treat the output as nullptr even if the handler
 *  eventually completes. */
static void
on_node_timeout(uv_timer_t* handle)
{
	node_work_t* work = (node_work_t*)handle->data;
	work->is_timed_out = 1;
	printf(
	    "[Workflow] Node '%s' timed out after %dms\n", work->node->id, work->node->timeout_ms);
}

/** @brief Internal: enqueue a workflow node for execution on the libuv
 *  thread pool.
 *
 * Algorithm:
 * 1. Check termination flag (budget exceeded, TTL expired).
 * 2. Handle Interactive Nodes: if a node is marked as interactive and
 *    has not yet been approved in this context, set is_paused flag,
 *    log WF_EV_PAUSE to WAL, broadcast to monitors, and return without
 *    executing.
 * 3. Log WF_EV_NODE_START to WAL and broadcast "node_queued" to monitors.
 * 4. Allocate a node_work_t struct. If tracing is active, create a
 *    trace node with start time and input dump.
 * 5. Increment total_executions and nodes_active counters.
 * 6. If the node has a per-node timeout, initialize and arm a uv_timer.
 * 7. Queue the work via uv_queue_work(). The node handler runs on a
 *    background thread; after_worker_cb processes the result.
 *
 * @param ctx   Workflow execution context.
 * @param node  The node to execute.
 * @param input Input data to pass to the node's handler. */
static void
execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input)
{
	uv_mutex_lock(&ctx->mutex);
	if (ctx->is_terminated) {
		uv_mutex_unlock(&ctx->mutex);
		return;
	}

	// 1. Handle Interactive Nodes (Pause)
	if (node->is_interactive && !ctx->node_approved[node->index]) {
		ctx->is_paused = 1;
		uv_mutex_unlock(&ctx->mutex);

		wal_log_event(ctx, WF_EV_PAUSE, node->id, input);
		_wf_broadcast(
		    ctx->wf, "workflow_paused", node->id, input ? (char*)input->value : nullptr);
		printf("[Workflow] Execution %s paused at node '%s'\n", ctx->exec_id, node->id);
		return;
	}

	// 2. Handle Remote Nodes (MQ Offload)
	if (node->is_remote && ctx->wf->mq && !ctx->node_approved[node->index]) {
		ctx->is_paused = 1;
		// We increment nodes_active even for remote tasks so the workflow doesn't
		// finish while waiting for MQ. It acts as an "outstanding" task.
		ctx->nodes_active++;
		uv_mutex_unlock(&ctx->mutex);

		cJSON* task = cJSON_CreateObject();
		cJSON_AddStringToObject(task, "exec_id", ctx->exec_id);
		cJSON_AddStringToObject(task, "node_id", node->id);
		if (input && input->value) {
			cJSON_AddStringToObject(task, "input", (char*)input->value);
		}
		char* json = cJSON_PrintUnformatted(task);

		csilk_mq_publish(ctx->wf->mq, "csilk.wf.tasks", json, strlen(json));
		wal_log_event(ctx, WF_EV_PAUSE, node->id, input);
		_wf_broadcast(ctx->wf, "node_remote_queued", node->id, json);

		printf(
		    "[Workflow] Execution %s offloaded node '%s' to MQ\n", ctx->exec_id, node->id);

		free(json);
		cJSON_Delete(task);
		return;
	}
	uv_mutex_unlock(&ctx->mutex);

	wal_log_event(ctx, WF_EV_NODE_START, node->id, nullptr);
	_wf_broadcast(ctx->wf, "node_queued", node->id, nullptr);

	node_work_t* work = calloc(1, sizeof(node_work_t));
	if (ctx->trace) {
		csilk_wf_trace_node_t* tn = calloc(1, sizeof(csilk_wf_trace_node_t));
		tn->node_id = strdup(node->id);
		tn->start_time = uv_hrtime() / 1000;
		tn->input_dump = strdup(input && input->value ? (char*)input->value : "(null)");
		work->trace_node = tn;
	}

	uv_mutex_lock(&ctx->mutex);
	ctx->total_executions++;
	ctx->nodes_active++;
	uv_mutex_unlock(&ctx->mutex);

	work->req.data = work;
	work->ctx = ctx;
	work->node = node;
	work->input = input;

	if (node->timeout_ms > 0) {
		uv_timer_init(ctx->wf->loop, &work->node_timer);
		work->node_timer.data = work;
		uv_timer_start(&work->node_timer, on_node_timeout, node->timeout_ms, 0);
	}

	uv_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

const char*
csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input, void (*callback)(csilk_data_t* result))
{
	return csilk_wf_run_ext_internal(wf, input, callback, nullptr);
}

void
csilk_wf_run_traced(csilk_wf_t* wf,
		    csilk_data_t* input,
		    void (*callback)(csilk_data_t* result, csilk_wf_trace_t* trace))
{
	csilk_wf_run_ext_internal(wf, input, nullptr, callback);
}

static void
on_workflow_ttl(uv_timer_t* handle)
{
	csilk_wf_ctx_t* ctx = (csilk_wf_ctx_t*)handle->data;
	uv_mutex_lock(&ctx->mutex);
	ctx->is_terminated = 1;
	ctx->is_ttl_expired = 1;
	uv_mutex_unlock(&ctx->mutex);
	printf("[Workflow] TTL Expired for execution %s\n", ctx->exec_id);
}

static const char*
csilk_wf_run_ext_internal(csilk_wf_t* wf,
			  csilk_data_t* input,
			  void (*callback)(csilk_data_t*),
			  void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*))
{
	if (!wf || wf->node_count == 0) {
		if (callback) {
			callback(nullptr);
		}
		if (trace_cb) {
			trace_cb(nullptr, nullptr);
		}
		return nullptr;
	}
	csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
	ctx->wf = wf;
	ctx->initial_input = input;
	ctx->callback = callback;
	ctx->trace_callback = trace_cb;
	ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
	ctx->node_approved = calloc(wf->node_count, sizeof(int));
	ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
	ctx->arena = csilk_arena_new(0);
	uv_mutex_init(&ctx->mutex);
	uv_mutex_init(&ctx->arena_mutex);
	uv_mutex_init(&ctx->trace_mutex);
	csilk_generate_uuid(ctx->exec_id);

	register_active_ctx(wf, ctx);

	if (wf->ttl_sec > 0) {
		uv_timer_init(wf->loop, &ctx->ttl_timer);
		ctx->ttl_timer.data = ctx;
		uv_timer_start(&ctx->ttl_timer, on_workflow_ttl, wf->ttl_sec * 1000, 0);
	}

	_wf_broadcast(wf, "workflow_start", ctx->exec_id, input ? (char*)input->value : nullptr);

	char* m_graph = csilk_wf_to_mermaid(wf);
	_wf_broadcast(wf, "workflow_topology", nullptr, m_graph);
	free(m_graph);
	if (wf->wal_dir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, ctx->exec_id);
		ctx->wal_path = strdup(path);
		wal_log_event(ctx, WF_EV_START, nullptr, input);
	}
	if (trace_cb) {
		ctx->trace = calloc(1, sizeof(csilk_wf_trace_t));
		ctx->trace->exec_id = strdup(ctx->exec_id);
		ctx->trace->start_time = uv_hrtime() / 1000;
	}
	int started = 0;
	for (size_t i = 0; i < wf->node_count; i++) {
		if (wf->nodes[i]->is_entry) {
			execute_node(ctx, wf->nodes[i], input);
			started = 1;
		}
	}
	if (!started) {
		for (size_t i = 0; i < wf->node_count; i++) {
			if (wf->nodes[i]->incoming_count == 0) {
				execute_node(ctx, wf->nodes[i], input);
				started = 1;
			}
		}
	}
	if (!started) {
		if (callback) {
			callback(nullptr);
		}
		if (trace_cb) {
			trace_cb(nullptr, nullptr);
		}
		cleanup_ctx(ctx);
		return nullptr;
	}
	return ctx->exec_id;
}

void
csilk_wf_resume(csilk_wf_t* wf, const char* exec_id, void (*callback)(csilk_data_t* result))
{
	if (!wf || !exec_id || !wf->wal_dir) {
		return;
	}
	char wal_path[512];
	snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
	FILE* f = fopen(wal_path, "rb");
	if (!f) {
		return;
	}
	csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
	ctx->wf = wf;
	ctx->callback = callback;
	ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
	ctx->node_approved = calloc(wf->node_count, sizeof(int));
	ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
	ctx->arena = csilk_arena_new(0);
	uv_mutex_init(&ctx->mutex);
	uv_mutex_init(&ctx->arena_mutex);
	uv_mutex_init(&ctx->trace_mutex);
	strncpy(ctx->exec_id, exec_id, sizeof(ctx->exec_id) - 1);
	ctx->exec_id[sizeof(ctx->exec_id) - 1] = '\0';
	ctx->wal_path = strdup(wal_path);
	int *node_started = calloc(wf->node_count, sizeof(int)),
	    *node_finished = calloc(wf->node_count, sizeof(int));
	int wf_ended = 0;
	csilk_wf_wal_header_t header;
	while (fread(&header, sizeof(header), 1, f) == 1) {
		if (header.magic != CSILK_WF_MAGIC) {
			break;
		}
		char* payload = header.payload_len > 0 ? malloc(header.payload_len) : nullptr;
		if (payload && fread(payload, header.payload_len, 1, f) != 1) {
			free(payload);
			break;
		}
		switch (header.type) {
		case WF_EV_NODE_START:
			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, payload) == 0) {
					node_started[i] = 1;
					break;
				}
			}
			break;
		case WF_EV_NODE_FINISH: {
			char* node_id = payload;
			size_t nid_len = strlen(node_id);
			if (nid_len + 1 >= header.payload_len) {
				break;
			}
			char* data_type = node_id + nid_len + 1;
			size_t dtype_len = strlen(data_type);
			if (nid_len + 1 + dtype_len + 1 >= header.payload_len) {
				break;
			}
			char* data_val = data_type + dtype_len + 1;

			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, node_id) == 0) {
					node_finished[i] = 1;
					ctx->node_outputs[i] = csilk_wf_data_new(
					    ctx, data_type, csilk_wf_strdup(ctx, data_val));
					csilk_wf_node_t* n = wf->nodes[i];
					for (size_t j = 0; j < n->edge_count; j++) {
						ctx->node_input_counts[n->edges[j].target->index]++;
					}
					break;
				}
			}
			break;
		}
		case WF_EV_PAUSE: {
			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, payload) == 0) {
					ctx->is_paused = 1;
					break;
				}
			}
			break;
		}
		case WF_EV_END:
			wf_ended = 1;
			break;
		}
		free(payload);
	}
	fclose(f);
	if (wf_ended) {
		if (callback) {
			callback(nullptr);
		}
		cleanup_ctx(ctx);
	} else if (ctx->is_paused) {
		cleanup_ctx(ctx);
	} else {
		for (size_t i = 0; i < wf->node_count; i++) {
			csilk_wf_node_t* n = wf->nodes[i];
			int trigger = 0;
			csilk_data_t* trigger_input = ctx->initial_input;
			if (node_started[i] && !node_finished[i]) {
				trigger = 1;
			} else if (!node_started[i]) {
				int threshold = n->incoming_count == 0 ? 1 : n->incoming_count;
				if (ctx->node_input_counts[n->index] >= threshold) {
					trigger = 1;
				}
			}
			if (trigger) {
				for (size_t j = 0; j < wf->node_count; j++) {
					csilk_wf_node_t* p = wf->nodes[j];
					for (size_t k = 0; k < p->edge_count; k++) {
						if (p->edges[k].target == n && node_finished[j]) {
							trigger_input = ctx->node_outputs[j];
							break;
						}
					}
				}
				execute_node(ctx, n, trigger_input);
			}
		}
	}
	free(node_started);
	free(node_finished);
}

void
csilk_wf_signal_continue(csilk_wf_t* wf,
			 const char* exec_id,
			 csilk_data_t* input,
			 void (*callback)(csilk_data_t* result))
{
	if (!wf || !exec_id || !wf->wal_dir) {
		return;
	}

	char wal_path[512];
	snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
	FILE* f = fopen(wal_path, "rb");
	if (!f) {
		return;
	}

	csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
	ctx->wf = wf;
	ctx->callback = callback;
	ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
	ctx->node_approved = calloc(wf->node_count, sizeof(int));
	ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
	ctx->arena = csilk_arena_new(0);
	uv_mutex_init(&ctx->mutex);
	uv_mutex_init(&ctx->arena_mutex);
	uv_mutex_init(&ctx->trace_mutex);
	strncpy(ctx->exec_id, exec_id, sizeof(ctx->exec_id) - 1);
	ctx->exec_id[sizeof(ctx->exec_id) - 1] = '\0';
	ctx->wal_path = strdup(wal_path);

	char* paused_node_id = nullptr;

	csilk_wf_wal_header_t header;
	while (fread(&header, sizeof(header), 1, f) == 1) {
		if (header.magic != CSILK_WF_MAGIC) {
			break;
		}
		char* payload = header.payload_len > 0 ? malloc(header.payload_len) : nullptr;
		if (payload && fread(payload, header.payload_len, 1, f) != 1) {
			free(payload);
			break;
		}
		switch (header.type) {
		case WF_EV_NODE_FINISH: {
			char* node_id = payload;
			size_t nid_len = strlen(node_id);
			if (nid_len + 1 >= header.payload_len) {
				break;
			}
			char* data_type = node_id + nid_len + 1;
			size_t dtype_len = strlen(data_type);
			if (nid_len + 1 + dtype_len + 1 >= header.payload_len) {
				break;
			}
			char* data_val = data_type + dtype_len + 1;

			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, node_id) == 0) {
					ctx->node_outputs[i] = csilk_wf_data_new(
					    ctx, data_type, csilk_wf_strdup(ctx, data_val));
					csilk_wf_node_t* n = wf->nodes[i];
					for (size_t j = 0; j < n->edge_count; j++) {
						ctx->node_input_counts[n->edges[j].target->index]++;
					}
					break;
				}
			}
			break;
		}
		case WF_EV_PAUSE:
			free(paused_node_id);
			paused_node_id = strdup(payload);
			break;
		case WF_EV_END:
			break;
		}
		free(payload);
	}
	fclose(f);

	if (paused_node_id) {
		csilk_wf_node_t* n = csilk_wf_get_node(wf, paused_node_id);
		if (n) {
			ctx->node_approved[n->index] = 1;
			execute_node(ctx, n, input);
		}
		free(paused_node_id);
	} else {
		cleanup_ctx(ctx);
	}
}
