
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
	void (*user_data_free)(void*);	    /**< Called by _wf_node_free to free user_data. */
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
 *  _wf_run_ext_internal() and freed by _wf_cleanup_ctx(). */
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
 *  Allocated in _wf_execute_node(), freed in after_worker_cb(). */
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
static void _wf_execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);
static void _wf_cleanup_ctx(csilk_wf_ctx_t* ctx);
static void after_worker_cb(uv_work_t* req, int status);

static void _wf_register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx);
static void _wf_unregister_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx);
static csilk_wf_ctx_t* _wf_find_active_ctx(csilk_wf_t* wf, const char* exec_id);

static const char* _wf_run_ext_internal(csilk_wf_t* wf,
					csilk_data_t* input,
					void (*callback)(csilk_data_t*),
					void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*));

/* --- Lifecycle --- */

#include <sys/stat.h>
#include <unistd.h>

static void
_wf_serve_ui_handler(csilk_ctx_t* c)
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
	csilk_app_get(app, path, _wf_serve_ui_handler);
}

csilk_wf_t*
