/**
 * @file wf_run.c
 * @brief Workflow run orchestration: entry points, TTL timer, start dispatch.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "workflow_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"

/* Forward declarations for cross-module calls */
extern void _wf_wal_log_event(csilk_wf_ctx_t*       ctx,
                              csilk_wf_event_type_t type,
                              const char*           node_id,
                              csilk_data_t*         data);
extern void _wf_cleanup_ctx(csilk_wf_ctx_t* ctx);
extern void register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx);
extern void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);

/**
 * @brief Runs the workflow definition asynchronously.
 *
 * Instantiates a new execution context, generates a unique UUID, initializes
 * tracking arrays, triggers entry nodes (nodes with 0 incoming dependencies or explicitly marked),
 * and kicks off async execution on the default I/O event loop.
 *
 * @param wf       The workflow definition instance.
 * @param input    The initial workflow input data.
 * @param callback Callback function invoked with the final workflow output when complete.
 * @return The unique execution ID string (UUID) assigned to this run. Do not free.
 */
const char*
csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input, void (*callback)(csilk_data_t* result))
{
    return _wf_run_ext_internal(wf, input, callback, NULL);
}

/**
 * @brief Runs the workflow definition asynchronously with detailed execution tracing.
 *
 * Similar to csilk_wf_run, but collects execution times, token counts, and input/output
 * value dumps for every node, returning a complete trace structure in the callback.
 *
 * @param wf       The workflow definition instance.
 * @param input    The initial workflow input data.
 * @param callback Callback function invoked with the final workflow output and execution trace.
 */
void
csilk_wf_run_traced(csilk_wf_t*   wf,
                    csilk_data_t* input,
                    void (*callback)(csilk_data_t* result, csilk_wf_trace_t* trace))
{
    _wf_run_ext_internal(wf, input, NULL, callback);
}

/**
 * @brief Timer callback invoked when the global workflow TTL expires.
 *
 * Sets the is_terminated and is_ttl_expired flags on the context, halting
 * any further node execution.
 *
 * @param handle The global TTL timer handle.
 */
static void
on_workflow_ttl(csilk_io_timer_t* handle)
{
    csilk_wf_ctx_t* ctx = (csilk_wf_ctx_t*)handle->data;
    csilk_mutex_lock(&ctx->mutex);
    ctx->is_terminated = 1;
    ctx->is_ttl_expired = 1;
    csilk_mutex_unlock(&ctx->mutex);
    CSILK_LOG_W("[Workflow] TTL Expired for execution %s", ctx->exec_id);
}

/**
 * @brief Internal common entry point for running workflows.
 *
 * Coordinates context creation, registration, active TTL timer setup, Mermaid graph
 * topology broadcast to monitors, WAL file initialization, and entry node execution.
 *
 * @param wf       The workflow definition instance.
 * @param input    The initial input data container.
 * @param callback The completion callback (for non-traced runs).
 * @param trace_cb The completion callback (for traced runs).
 * @return The assigned execution UUID, or NULL on failure.
 */
const char*
_wf_run_ext_internal(csilk_wf_t*   wf,
                     csilk_data_t* input,
                     void (*callback)(csilk_data_t*),
                     void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*))
{
    if (!wf || wf->node_count == 0) {
        if (callback) {
            callback(NULL);
        }
        if (trace_cb) {
            trace_cb(NULL, NULL);
        }
        return NULL;
    }
    csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
    atomic_init(&ctx->nodes_active, 0);
    ctx->wf = wf;
    ctx->initial_input = input;
    ctx->callback = callback;
    ctx->trace_callback = trace_cb;
    ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
    ctx->node_approved = calloc(wf->node_count, sizeof(int));
    ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
    ctx->arena = csilk_arena_new(0);
    csilk_mutex_init(&ctx->mutex);
    csilk_mutex_init(&ctx->arena_mutex);
    csilk_mutex_init(&ctx->trace_mutex);
    csilk_generate_uuid(ctx->exec_id);

    register_active_ctx(wf, ctx);

    if (wf->ttl_sec > 0) {
        csilk_io_timer_init(wf->loop, &ctx->ttl_timer);
        ctx->ttl_timer.data = ctx;
        csilk_io_timer_start(&ctx->ttl_timer, on_workflow_ttl, wf->ttl_sec * 1000, 0);
    }

    _wf_broadcast(wf, "workflow_start", ctx->exec_id, input ? (char*)input->value : NULL);

    char* m_graph = csilk_wf_to_mermaid(wf);
    _wf_broadcast(wf, "workflow_topology", NULL, m_graph);
    free(m_graph);
    if (wf->wal_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, ctx->exec_id);
        ctx->wal_path = strdup(path);
        _wf_wal_log_event(ctx, WF_EV_START, NULL, input);
    }
    if (trace_cb) {
        ctx->trace = calloc(1, sizeof(csilk_wf_trace_t));
        ctx->trace->exec_id = strdup(ctx->exec_id);
        ctx->trace->start_time = csilk_io_hrtime() / 1000;
    }
    int                  started = 0;
    static __thread char ret_exec_id[CSILK_UUID_BUF_SIZE];
    snprintf(ret_exec_id, sizeof(ret_exec_id), "%s", ctx->exec_id);

    atomic_fetch_add(&ctx->nodes_active, 1);

    for (size_t i = 0; i < wf->node_count; i++) {
        if (wf->nodes[i]->is_entry) {
            execute_node(ctx, wf->nodes[i], input);
            started = 1;
        }
    }
    if (!started) {
        /* Implicit single entry: start only the FIRST zero-incoming node.
         * Starting all of them races parallel branches against each other —
         * with a dynamic router the entry node alone determines the path,
         * and sibling roots would complete spuriously and win the output. */
        for (size_t i = 0; i < wf->node_count; i++) {
            if (wf->nodes[i]->incoming_count == 0) {
                execute_node(ctx, wf->nodes[i], input);
                started = 1;
                break;
            }
        }
    }

    int active = atomic_fetch_sub(&ctx->nodes_active, 1) - 1;

    if (!started) {
        if (callback) {
            callback(NULL);
        }
        if (trace_cb) {
            trace_cb(NULL, NULL);
        }
        if (active == 0) {
            _wf_cleanup_ctx(ctx);
        }
        return NULL;
    }

    if (active == 0) {
        _wf_cleanup_ctx(ctx);
    }

    return ret_exec_id;
}
