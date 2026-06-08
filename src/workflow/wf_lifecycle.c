/**
 * @file wf_lifecycle.c
 * @brief Workflow lifecycle management: creation, destruction, node/edge
 *        configuration, persistence, budget, and distributed execution.
 */

#include "workflow_internal.h"

#include <sys/stat.h>
#include <unistd.h>

/* --- UI Handler --- */

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

/**
 * @brief Registers a HTTP GET route on the server application to serve the workflow dashboard UI.
 *
 * Checks several local and prefix paths for the workflow_ui.html template file.
 * Returns 404 if the HTML template is not found.
 *
 * @param app  Pointer to the csilk app context.
 * @param path Optional custom URL path route (defaults to "/workflow/ui" if null).
 */
void
csilk_wf_serve_ui(csilk_app_t* app, const char* path)
{
	csilk_app_get(app, path ? path : "/workflow/ui", serve_ui_handler);
}

/* --- Lifecycle --- */

/**
 * @brief Creates a new empty workflow configuration engine with the given name.
 *
 * Initializes sync mutexes and sets the execution event loop to uv_default_loop().
 *
 * @param name The descriptive name of the workflow.
 * @return Pointer to the new workflow handle, or nullptr on allocation failure.
 */
csilk_wf_t*
csilk_wf_new(const char* name)
{
	csilk_wf_t* wf = calloc(1, sizeof(csilk_wf_t));
	if (!wf) {
		return nullptr;
	}
	wf->name = strdup(name);
	wf->loop = uv_default_loop();
	uv_mutex_init(&wf->monitor_mutex);
	uv_mutex_init(&wf->ctx_mutex);
	return wf;
}

static void
node_free(csilk_wf_node_t* node)
{
	if (!node) {
		return;
	}
	for (size_t i = 0; i < node->edge_count; i++) {
		free(node->edges[i].condition);
	}
	free(node->edges);
	free(node->id);
	free(node->output_schema);
	if (node->user_data_free) {
		node->user_data_free(node->user_data);
	}
	free(node);
}

static void
ai_config_free(void* ptr)
{
	csilk_ai_config_t* c = (csilk_ai_config_t*)ptr;
	free(c->model);
	free(c->system_msg);
	free(c->prompt);
	free(c);
}

static void
vector_search_config_free(void* ptr)
{
	csilk_vector_search_config_t* c = (csilk_vector_search_config_t*)ptr;
	free(c->embedding_model);
	free(c->collection);
	free(c->input_template);
	free(c);
}

/**
 * @brief Deallocates all resources occupied by a workflow structure.
 *
 * Frees nodes, edges, static tools registries, WebSocket monitors lists,
 * mutexes, and all active execution contexts.
 *
 * @param wf The workflow handle to free.
 */
void
csilk_wf_free(csilk_wf_t* wf)
{
	if (!wf) {
		return;
	}
	while (wf->active_context_count > 0) {
		_wf_cleanup_ctx(wf->active_contexts[0]);
	}
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
	for (size_t i = 0; i < wf->monitor_count; i++) {
		wf->monitors[i] = nullptr;
	}
	free(wf->monitors);
	uv_mutex_destroy(&wf->monitor_mutex);
	free(wf->active_contexts);
	uv_mutex_destroy(&wf->ctx_mutex);
	free(wf->wal_dir);
	free(wf->name);
	free(wf);
}

/**
 * @brief Appends a new step node to the workflow DAG.
 *
 * Allocates and registers a node configuration.
 *
 * @param wf        The workflow instance.
 * @param id        Unique string identifier for the node.
 * @param handler   Callback executed when this node is run (null permitted for remote-only nodes).
 * @param user_data Opaque pointer forwarded to the callback.
 * @return The new node pointer, or nullptr on failure.
 */
csilk_wf_node_t*
csilk_wf_add(csilk_wf_t* wf, const char* id, csilk_wf_handler_t handler, void* user_data)
{
	if (!wf || !id) {
		return nullptr;
	}
	if (wf->node_count >= wf->node_capacity) {
		size_t new_cap = wf->node_capacity == 0 ? 16 : wf->node_capacity * 2;
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
	node->handler = handler;
	node->user_data = user_data;
	node->index = (int)wf->node_count;
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
add_edge(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to)
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
	csilk_wf_edge_t* e = &from->edges[from->edge_count++];
	e->condition = condition ? strdup(condition) : nullptr;
	e->target = to;
	to->incoming_count++;
}

/**
 * @brief Unconditionally binds one node to another (sequential execution).
 * @param from Source node.
 * @param to   Target node.
 */
void
csilk_wf_bind(csilk_wf_node_t* from, csilk_wf_node_t* to)
{
	add_edge(from, nullptr, to);
}

/**
 * @brief Binds nodes conditionally. The target node runs only if the source node's
 *        output type matches the condition string.
 * @param from      Source node.
 * @param condition Output type string to match.
 * @param to        Target node.
 */
void
csilk_wf_on(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to)
{
	add_edge(from, condition, to);
}

/**
 * @brief Helper identical to csilk_wf_on for creating conditional loops.
 * @param from      Source node.
 * @param condition Output type to match.
 * @param to        Target node.
 */
void
csilk_wf_on_loop(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to)
{
	add_edge(from, condition, to);
}

/**
 * @brief Directs execution to target if the source node returns nullptr (representing error).
 * @param from   Source node.
 * @param target Error fallback node.
 */
void
csilk_wf_on_error(csilk_wf_node_t* from, csilk_wf_node_t* target)
{
	if (from) {
		from->error_target = target;
	}
}

/**
 * @brief Configures a dynamic routing function callback on a node.
 * @param node   The node instance.
 * @param router Callback returning target node ID based on output data.
 */
void
csilk_wf_route(csilk_wf_node_t* node, csilk_wf_router_t router)
{
	if (node) {
		node->router_fn = router;
	}
}

/**
 * @brief Configures execution join policy on a node (AND join vs OR join).
 * @param node   Target node.
 * @param policy Join policy value.
 */
void
csilk_wf_node_set_join(csilk_wf_node_t* node, csilk_wf_join_policy_t policy)
{
	if (node) {
		node->join_policy = policy;
	}
}

/* --- Persistence --- */

/**
 * @brief Enables Write-Ahead Log (WAL) persistence for the workflow and sets the directory path.
 * @param wf      The workflow instance.
 * @param wal_dir Directory path where execution WAL files will be stored.
 */
void
csilk_wf_set_persistence(csilk_wf_t* wf, const char* wal_dir)
{
	if (!wf || !wal_dir) {
		return;
	}
	wf->wal_dir = strdup(wal_dir);
	mkdir(wal_dir, 0755);
}

/* --- Budget --- */

/**
 * @brief Sets a maximum token budget for workflow executions to prevent runaway LLM costs.
 * @param wf         The workflow instance.
 * @param max_tokens Maximum token count allowed.
 */
void
csilk_wf_set_budget(csilk_wf_t* wf, int max_tokens)
{
	if (wf) {
		wf->max_tokens = max_tokens;
	}
}

/**
 * @brief Sets an execution timeout limit on a specific node.
 * @param node       The node to configure.
 * @param timeout_ms Timeout value in milliseconds.
 */
void
csilk_wf_node_set_timeout(csilk_wf_node_t* node, int timeout_ms)
{
	if (node) {
		node->timeout_ms = timeout_ms;
	}
}

/**
 * @brief Sets a time-to-live expiration limit on workflow execution contexts.
 * @param wf      The workflow instance.
 * @param ttl_sec TTL value in seconds.
 */
void
csilk_wf_set_ttl(csilk_wf_t* wf, int ttl_sec)
{
	if (wf) {
		wf->ttl_sec = ttl_sec;
	}
}

/**
 * @brief Marks a node as interactive (requiring human-in-the-loop approval before running).
 * @param node           Target node.
 * @param is_interactive Boolean flag.
 */
void
csilk_wf_node_set_interactive(csilk_wf_node_t* node, int is_interactive)
{
	if (node) {
		node->is_interactive = is_interactive;
	}
}

/**
 * @brief Binds a JSON schema schema validator to a node's output data structure.
 * @param node   Target node.
 * @param schema JSON schema string.
 */
void
csilk_wf_node_set_schema(csilk_wf_node_t* node, const char* schema)
{
	if (node) {
		free(node->output_schema);
		node->output_schema = schema ? strdup(schema) : nullptr;
	}
}

/**
 * @brief Configures execution retry parameters on a node.
 * @param node           Target node.
 * @param max_retries    Maximum retry attempts.
 * @param retry_delay_ms Delay interval in milliseconds.
 */
void
csilk_wf_node_set_retry(csilk_wf_node_t* node, int max_retries, int retry_delay_ms)
{
	if (node) {
		node->max_retries = max_retries;
		node->retry_delay_ms = retry_delay_ms;
	}
}

static csilk_data_t*
validate_schema(csilk_wf_node_t* node, csilk_data_t* output)
{
	(void)node;
	return output;
}

static csilk_data_t*
remote_pass_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	return input;
}

/**
 * @brief Configures a node to execute remotely via message queue offloading.
 * @param node      Target node.
 * @param is_remote Boolean flag.
 */
void
csilk_wf_node_set_remote(csilk_wf_node_t* node, int is_remote)
{
	if (node) {
		node->is_remote = is_remote;
		if (is_remote && node->handler == nullptr) {
			node->handler = remote_pass_handler;
		}
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
			csilk_wf_ctx_t* active = _wf_find_active_ctx(wf, exec_id);
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
					_wf_execute_node(active, n, out_data);
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

/**
 * @brief Enables distributed workflow support by binding a message queue connection.
 *
 * Subscribes to the "csilk.wf.results" topic to dynamically receive and resume offloaded tasks.
 *
 * @param wf The workflow instance.
 * @param mq The message queue connection handle.
 */
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

/* --- Node Lookup --- */

/**
 * @brief Looks up a workflow node pointer by its unique ID string.
 * @param wf The workflow instance.
 * @param id Unique identifier string of the target node.
 * @return The matching node pointer, or nullptr if not found.
 */
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
