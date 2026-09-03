/**
 * @file wf_node.c
 * @brief Node execution: worker callbacks, retry timers, dispatch logic.
 */

#include <stdatomic.h>
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

/** @brief Maximum workflow steps per definition. */
enum { MAX_WORKFLOW_STEPS = 1000 };

void        execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);
static void after_worker_cb(csilk_io_work_t* req, int status);

/** @brief Thread-pool work callback — executes a workflow node's
 *  handler on a background thread.
 *
 * Broadcasts a "node_start" event to monitors, then calls the node's
 * handler function. The output is stored in the work request struct
 * for retrieval by after_worker_cb on the main loop thread. */
static void
worker_cb(csilk_io_work_t* req)
{
    node_work_t* work = (node_work_t*)req->data;
    _wf_broadcast(work->ctx->wf, "node_start", work->node->id, NULL);
    work->output = work->node->handler(work->ctx, work->input, work->node->user_data);
}

/**
 * @brief Timer callback invoked when a node retry delay expires.
 *
 * Re-queues the node's execution on the thread pool.
 *
 * @param handle The timer handle.
 */
static void
on_retry_timer(csilk_io_timer_t* handle)
{
    node_work_t* work = (node_work_t*)handle->data;
    CSILK_LOG_I("[Workflow] Retrying node '%s' (attempt %d/%d)...",
                work->node->id,
                work->retry_count,
                work->node->max_retries);
    csilk_io_queue_work(work->ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

/**
 * @brief Handle close callback for node work timer.
 *
 * Frees the node_work_t allocation after its timer handle is closed.
 *
 * @param handle The closed handle.
 */
static void
on_work_timer_close(csilk_io_handle_t* handle)
{
    node_work_t* work = (node_work_t*)handle->data;
    free(work);
}

/**
 * @brief Safely cleans up the node work struct, closing timers if active.
 *
 * If the node has an active timeout timer, stops and closes it asynchronously,
 * deferring the free until on_work_timer_close. Otherwise, frees it immediately.
 *
 * @param work The node work structure to free.
 */
static void
free_work(node_work_t* work)
{
    if (work->timer_initialized && !work->timer_closing) {
        work->timer_closing = 1;
        csilk_io_timer_stop(&work->node_timer);
        work->node_timer.data = work;
        csilk_io_close((csilk_io_handle_t*)&work->node_timer, on_work_timer_close);
        return;
    }
    free(work);
}

/** @brief After-work callback — processes node completion on the
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
 * 6. If output is NULL and error_target is set, route to error node.
 * 7. If the node has a dynamic router function, call it to determine
 *    the next node; otherwise, evaluate each outgoing edge:
 *    - Unconditional edges (condition == NULL) always match.
 *    - Conditional edges match if output type equals condition string.
 *    - Dynamic routers can inspect previous node outputs from ctx.
 * 8. For matching edges, check the target's join policy: AND join
 *    requires all incoming edges to fire before the target is ready;
 *    OR join fires on any single edge.
 * 9. If no edges are triggered and no nodes are active, the workflow
 *    is complete: log WF_EV_END, deliver the final output via callback,
 *    and clean up the context. */
static void
after_worker_cb(csilk_io_work_t* req, int status)
{
    (void)status;
    node_work_t*     work = (node_work_t*)req->data;
    csilk_wf_ctx_t*  ctx = work->ctx;
    csilk_wf_node_t* node = work->node;
    csilk_data_t*    output = work->output;

    if (work->timer_initialized && !work->is_timed_out) {
        csilk_io_timer_stop(&work->node_timer);
    }

    if (work->is_timed_out) {
        output = NULL;
    }

    // Handle Retries before error logic
    if (output == NULL && work->retry_count < node->max_retries) {
        work->retry_count++;
        CSILK_LOG_W(
            "[Workflow] Node '%s' failed, scheduled retry in %dms", node->id, node->retry_delay_ms);

        if (node->retry_delay_ms > 0) {
            if (!work->timer_initialized) {
                csilk_io_timer_init(ctx->wf->loop, &work->node_timer);
                work->node_timer.data = work;
                work->timer_initialized = 1;
            }
            csilk_io_timer_start(&work->node_timer, on_retry_timer, node->retry_delay_ms, 0);
            return; // Wait for timer
        } else {
            csilk_io_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
            return;
        }
    }

    // JSON Schema Validation

    if (output && output->value && node->output_schema) {
        csilk_json_t* schema = csilk_json_parse(node->output_schema);
        csilk_json_t* data = csilk_json_parse((char*)output->value);
        if (schema && data) {
            csilk_json_t* required = csilk_json_get(schema, "required");
            if (csilk_json_is_array(required)) {
                for (int i = 0; i < csilk_json_array_size(required); i++) {
                    csilk_json_t* field = csilk_json_array_get(required, i);
                    if (csilk_json_is_string(field) &&
                        !csilk_json_get(data, csilk_json_string_value(field))) {
                        CSILK_LOG_W("[Workflow] Node '%s' output failed "
                                    "schema: missing required field '%s'",
                                    node->id,
                                    csilk_json_string_value(field));
                        output = NULL;
                        break;
                    }
                }
            }
        } else if (node->output_schema) {
            output = NULL; // Invalid JSON or Schema
        }
        csilk_json_free(schema);
        csilk_json_free(data);
    }

    csilk_mutex_lock(&ctx->mutex);

    ctx->node_outputs[node->index] = output;
    if (output && output->meta) {
        csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
        ctx->total_tokens += am->prompt_tokens + am->completion_tokens;
    }
    csilk_mutex_unlock(&ctx->mutex);

    _wf_wal_log_event(ctx, WF_EV_NODE_FINISH, node->id, output);
    _wf_broadcast(ctx->wf, "node_finish", node->id, output ? (char*)output->value : NULL);

    if (work->trace_node) {
        work->trace_node->end_time = csilk_io_hrtime() / 1000;
        if (output) {
            work->trace_node->output_dump = strdup(output->value ? (char*)output->value : "(null)");
            if (output->meta) {
                csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
                work->trace_node->model = strdup(am->model);
                work->trace_node->prompt_tokens = am->prompt_tokens;
                work->trace_node->completion_tokens = am->completion_tokens;
            }
        }
        csilk_mutex_lock(&ctx->trace_mutex);
        ctx->trace->nodes = realloc(ctx->trace->nodes,
                                    sizeof(csilk_wf_trace_node_t*) * (ctx->trace->node_count + 1));
        ctx->trace->nodes[ctx->trace->node_count++] = work->trace_node;
        csilk_mutex_unlock(&ctx->trace_mutex);
    }

    csilk_mutex_lock(&ctx->mutex);
    if (ctx->wf->max_tokens > 0 && ctx->total_tokens > ctx->wf->max_tokens) {
        ctx->is_terminated = 1;
        CSILK_LOG_E("[Workflow] Budget exceeded: %d > %d. Terminating.",
                    ctx->total_tokens,
                    ctx->wf->max_tokens);
    }
    int terminated = ctx->is_terminated;
    csilk_mutex_unlock(&ctx->mutex);

    if (terminated) {
        csilk_mutex_lock(&ctx->mutex);
        ctx->nodes_active--;
        int current_active = ctx->nodes_active;
        csilk_mutex_unlock(&ctx->mutex);
        if (current_active == 0) {
            /* Hold the workflow alive across the user callback: the callback
             * (e.g. a Python bridge) may free the workflow while we still
             * touch ctx->trace and unregister the context below. */
            csilk_wf_t* wf = ctx->wf;
            atomic_fetch_add(&wf->pending_completions, 1);
            if (ctx->trace_callback) {
                ctx->trace_callback(NULL, ctx->trace);
            } else if (ctx->callback) {
                ctx->callback(NULL);
            }
            if (ctx->trace_callback) {
                ctx->trace = NULL;
            }
            _wf_cleanup_ctx(ctx);
            atomic_fetch_sub(&wf->pending_completions, 1);
        }
        free_work(work);
        return;
    }

    if (output == NULL && node->error_target) {
        execute_node(ctx, node->error_target, NULL);
        csilk_mutex_lock(&ctx->mutex);
        ctx->nodes_active--;
        csilk_mutex_unlock(&ctx->mutex);
        free_work(work);
        return;
    }

    int triggered_count = 0;
    if (node->router_fn) {
        const char* target_id = node->router_fn(output);
        if (target_id) {
            CSILK_LOG_D("[Workflow] Dynamic router selected '%s'", target_id);
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
            int              match = 0;
            if (edge->condition == NULL) {
                match = 1;
            } else if (output && output->type && strcmp(output->type, edge->condition) == 0) {
                match = 1;
            }

            CSILK_LOG_D(
                "[Workflow] Evaluating edge %zu to '%s' (match=%d)", i, edge->target->id, match);

            if (match) {
                csilk_wf_node_t* target = edge->target;
                int              ready = 0;
                csilk_mutex_lock(&ctx->mutex);
                if (ctx->total_executions < MAX_WORKFLOW_STEPS) {
                    ctx->node_input_counts[target->index]++;
                    int threshold = target->incoming_count == 0 ? 1 : target->incoming_count;

                    CSILK_LOG_D("[Workflow] Node '%s' input count: %d/%d",
                                target->id,
                                ctx->node_input_counts[target->index],
                                threshold);

                    if (ctx->node_input_counts[target->index] >= threshold) {
                        ready = 1;
                        ctx->node_input_counts[target->index] = 0;
                    }
                }
                csilk_mutex_unlock(&ctx->mutex);
                if (ready) {
                    CSILK_LOG_D("[Workflow] Triggering node '%s'", target->id);
                    execute_node(ctx, target, output);
                    triggered_count++;
                }
            }
        }
    }

    csilk_mutex_lock(&ctx->mutex);
    ctx->nodes_active--;
    int current_active = ctx->nodes_active;
    csilk_mutex_unlock(&ctx->mutex);
    if (triggered_count == 0 && current_active == 0) {
        /* Hold the workflow alive across the user callback: the callback
         * (e.g. a Python bridge) may free the workflow while we still touch
         * ctx->trace, the WAL and the active-context registry below. */
        csilk_wf_t* wf = ctx->wf;
        atomic_fetch_add(&wf->pending_completions, 1);
        _wf_wal_log_event(ctx, WF_EV_END, NULL, NULL);
        _wf_broadcast(wf, "workflow_end", NULL, output ? (char*)output->value : NULL);
        if (ctx->trace) {
            ctx->trace->end_time = csilk_io_hrtime() / 1000;
        }
        if (ctx->trace_callback) {
            ctx->trace_callback(output, ctx->trace);
        } else if (ctx->callback) {
            ctx->callback(output);
        }
        if (ctx->trace_callback) {
            ctx->trace = NULL;
        } else if (ctx->trace) {
            csilk_wf_trace_free(ctx->trace);
            ctx->trace = NULL;
        }
        _wf_cleanup_ctx(ctx);
        atomic_fetch_sub(&wf->pending_completions, 1);
    }
    free_work(work);
}

/** @brief Timer callback — marks a node as timed out.
 *  Sets the is_timed_out flag on the node_work_t, which causes
 *  after_worker_cb to treat the output as NULL even if the handler
 *  eventually completes. */
static void
on_node_timeout(csilk_io_timer_t* handle)
{
    node_work_t* work = (node_work_t*)handle->data;
    work->is_timed_out = 1;
    CSILK_LOG_W(
        "[Workflow] Node '%s' timed out after %dms", work->node->id, work->node->timeout_ms);
}

/** @brief Internal: enqueue a workflow node for execution on the
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
 * 7. Queue the work via csilk_io_queue_work(). The node handler runs on a
 *    background thread; after_worker_cb processes the result.
 *
 * @param ctx   Workflow execution context.
 * @param node  The node to execute.
 * @param input Input data to pass to the node's handler. */
void
execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input)
{
    csilk_mutex_lock(&ctx->mutex);
    if (ctx->is_terminated) {
        csilk_mutex_unlock(&ctx->mutex);
        return;
    }

    // 1. Handle Interactive Nodes (Pause)
    if (node->is_interactive && !ctx->node_approved[node->index]) {
        ctx->is_paused = 1;
        csilk_mutex_unlock(&ctx->mutex);

        _wf_wal_log_event(ctx, WF_EV_PAUSE, node->id, input);
        _wf_broadcast(ctx->wf, "workflow_paused", node->id, input ? (char*)input->value : NULL);
        CSILK_LOG_I("[Workflow] Execution %s paused at node '%s'", ctx->exec_id, node->id);
        return;
    }

    // 2. Handle Remote Nodes (MQ Offload)
    if (node->is_remote && ctx->wf->mq && !ctx->node_approved[node->index]) {
        ctx->is_paused = 1;
        // We increment nodes_active even for remote tasks so the workflow doesn't
        // finish while waiting for MQ. It acts as an "outstanding" task.
        ctx->nodes_active++;
        csilk_mutex_unlock(&ctx->mutex);

        csilk_json_t* task = csilk_json_object();
        csilk_json_add_string(task, "exec_id", ctx->exec_id);
        csilk_json_add_string(task, "node_id", node->id);
        if (input && input->value) {
            csilk_json_add_string(task, "input", (char*)input->value);
        }
        char* json = csilk_json_serialize(task, NULL);

        csilk_mq_publish(ctx->wf->mq, "csilk.wf.tasks", json, strlen(json));
        _wf_wal_log_event(ctx, WF_EV_PAUSE, node->id, input);
        _wf_broadcast(ctx->wf, "node_remote_queued", node->id, json);

        CSILK_LOG_I("[Workflow] Execution %s offloaded node '%s' to MQ", ctx->exec_id, node->id);

        free(json);
        csilk_json_free(task);
        return;
    }
    csilk_mutex_unlock(&ctx->mutex);

    _wf_wal_log_event(ctx, WF_EV_NODE_START, node->id, NULL);
    _wf_broadcast(ctx->wf, "node_queued", node->id, NULL);

    node_work_t* work = calloc(1, sizeof(node_work_t));
    if (ctx->trace) {
        csilk_wf_trace_node_t* tn = calloc(1, sizeof(csilk_wf_trace_node_t));
        tn->node_id = strdup(node->id);
        tn->start_time = csilk_io_hrtime() / 1000;
        tn->input_dump = strdup(input && input->value ? (char*)input->value : "(null)");
        work->trace_node = tn;
    }

    csilk_mutex_lock(&ctx->mutex);
    ctx->total_executions++;
    ctx->nodes_active++;
    csilk_mutex_unlock(&ctx->mutex);

    work->req.data = work;
    work->ctx = ctx;
    work->node = node;
    work->input = input;

    if (node->timeout_ms > 0) {
        csilk_io_timer_init(ctx->wf->loop, &work->node_timer);
        work->node_timer.data = work;
        work->timer_initialized = 1;
        csilk_io_timer_start(&work->node_timer, on_node_timeout, node->timeout_ms, 0);
    }

    csilk_io_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

/**
 * @brief Internal wrapper to expose execute_node to other modules.
 *
 * @param ctx   Workflow execution context.
 * @param node  The node to execute.
 * @param input Input data to pass to the node.
 */
CSILK_INTERNAL void
_wf_execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input)
{
    execute_node(ctx, node, input);
}
