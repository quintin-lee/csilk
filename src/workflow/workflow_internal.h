/**
 * @file workflow_internal.h
 * @brief Internal data structures and private APIs for the workflow engine.
 */

#ifndef CSILK_WORKFLOW_INTERNAL_CORE_H
#define CSILK_WORKFLOW_INTERNAL_CORE_H

#include <uv.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "csilk/app/workflow.h"
#include "csilk/core/internal.h"
#include "csilk/app/workflow_wal.h"
#include "csilk/core/workflow_internal.h"
#include "cJSON.h"

/** @brief AI metadata attached to workflow node outputs. */
typedef struct {
	char* model;
	int prompt_tokens;
	int completion_tokens;
} csilk_ai_meta_t;

typedef struct csilk_wf_edge_s {
	char* condition;
	csilk_wf_node_t* target;
} csilk_wf_edge_t;

struct csilk_wf_node_s {
	char* id;
	int index;
	csilk_wf_handler_t handler;
	void* user_data;
	csilk_wf_edge_t* edges;
	size_t edge_count;
	size_t edge_capacity;
	int incoming_count;
	int is_entry;
	csilk_wf_node_t* error_target;
	csilk_wf_router_t router_fn;
	csilk_wf_join_policy_t join_policy;
	int timeout_ms;
	int is_interactive;
	char* output_schema;
	int max_retries;
	int retry_delay_ms;
	int is_remote;
	void (*user_data_free)(void*);
};

struct csilk_wf_s {
	char* name;
	csilk_wf_node_t** nodes;
	size_t node_count;
	size_t node_capacity;
	uv_loop_t* loop;
	char* wal_dir;
	csilk_wf_tool_entry_t* tools;
	size_t tool_count;
	size_t tool_capacity;
	csilk_wf_tool_discovery_fn tool_discovery;
	void* tool_discovery_user_data;
	csilk_ctx_t** monitors;
	size_t monitor_count;
	size_t monitor_capacity;
	uv_mutex_t monitor_mutex;
	int max_tokens;
	int ttl_sec;
	csilk_mq_t* mq;
	csilk_wf_ctx_t** active_contexts;
	size_t active_context_count;
	size_t active_context_capacity;
	uv_mutex_t ctx_mutex;
};

struct csilk_wf_ctx_s {
	csilk_wf_t* wf;
	csilk_data_t* initial_input;
	void (*callback)(csilk_data_t*);
	void (*trace_callback)(csilk_data_t*, csilk_wf_trace_t*);
	int* node_input_counts;
	int total_executions;
	int nodes_active;
	uv_mutex_t mutex;
	csilk_arena_t* arena;
	uv_mutex_t arena_mutex;
	csilk_data_t** node_outputs;
	char exec_id[CSILK_UUID_BUF_SIZE];
	char* wal_path;
	csilk_wf_trace_t* trace;
	uv_mutex_t trace_mutex;
	int total_tokens;
	int is_terminated;
	int is_paused;
	int* node_approved;
	uv_timer_t ttl_timer;
	int is_ttl_expired;
};

typedef struct node_work_s {
	uv_work_t req;
	csilk_wf_ctx_t* ctx;
	csilk_wf_node_t* node;
	csilk_data_t* input;
	csilk_data_t* output;
	csilk_ai_meta_t* ai_meta;
	uv_timer_t node_timer;
	int is_timed_out;
	int retry_count;
	int timer_closing;
	csilk_wf_trace_node_t* trace_node;
} node_work_t;

/* --- Internal APIs --- */
CSILK_INTERNAL void
_wf_broadcast(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload);
CSILK_INTERNAL void
_wf_execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);
CSILK_INTERNAL void _wf_cleanup_ctx(csilk_wf_ctx_t* ctx);
CSILK_INTERNAL csilk_wf_ctx_t* _wf_find_active_ctx(csilk_wf_t* wf, const char* exec_id);
CSILK_INTERNAL const char* _wf_run_ext_internal(csilk_wf_t* wf,
						csilk_data_t* input,
						void (*callback)(csilk_data_t*),
						void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*));

#endif
