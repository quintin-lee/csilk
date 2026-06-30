/**
 * @file wf_scheduler.c
 * @brief Workflow DAG scheduler: node execution, after-completion dispatch,
 *        WAL event logging, run/resume/signal entry points.
 */

#include <stdatomic.h>

#include "workflow_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"

/* --- Active Context Management --- */

/**
 * @brief Registers a newly created execution context as active.
 *
 * Adds the context pointer to the workflow's active contexts array in a thread-safe manner.
 *
 * @param wf  The workflow definition instance.
 * @param ctx The execution context to register.
 */
static void
register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
	csilk_mutex_lock(&wf->ctx_mutex);
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
	csilk_mutex_unlock(&wf->ctx_mutex);
}

/**
 * @brief Unregisters an active execution context.
 *
 * Removes the context pointer from the workflow's active contexts array in a thread-safe manner.
 *
 * @param wf  The workflow definition instance.
 * @param ctx The execution context to unregister.
 */
static void
unregister_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
	csilk_mutex_lock(&wf->ctx_mutex);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		if (wf->active_contexts[i] == ctx) {
			wf->active_contexts[i] = wf->active_contexts[--wf->active_context_count];
			break;
		}
	}
	csilk_mutex_unlock(&wf->ctx_mutex);
}

/**
 * @brief Locates an active execution context by its execution ID.
 *
 * Performs a linear scan of the active contexts array in a thread-safe manner.
 *
 * @param wf      The workflow definition instance.
 * @param exec_id The unique execution UUID to find.
 * @return A pointer to the matching csilk_wf_ctx_t, or nullptr if not found.
 */
static csilk_wf_ctx_t*
find_active_ctx(csilk_wf_t* wf, const char* exec_id)
{
	csilk_wf_ctx_t* found = nullptr;
	csilk_mutex_lock(&wf->ctx_mutex);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		if (strcmp(wf->active_contexts[i]->exec_id, exec_id) == 0) {
			found = wf->active_contexts[i];
			break;
		}
	}
	csilk_mutex_unlock(&wf->ctx_mutex);
	return found;
}

static void
cleanup_stale_ctx(csilk_wf_t* wf, const char* exec_id)
{
	csilk_wf_ctx_t* stale = find_active_ctx(wf, exec_id);
	if (stale) {
		csilk_mutex_lock(&stale->mutex);
		stale->is_terminated = 1;
		int active = stale->nodes_active;
		csilk_mutex_unlock(&stale->mutex);
		if (active == 0) {
			_wf_cleanup_ctx(stale);
		} else {
			unregister_active_ctx(wf, stale);
		}
	}
}

/**
 * @brief Internal wrapper to expose find_active_ctx to other modules.
 *
 * @param wf      The workflow definition instance.
 * @param exec_id The execution UUID to find.
 * @return A pointer to the active context, or nullptr if not found.
 */
CSILK_INTERNAL csilk_wf_ctx_t*
_wf_find_active_ctx(csilk_wf_t* wf, const char* exec_id)
{
	return find_active_ctx(wf, exec_id);
}

/* --- Context Cleanup --- */

/**
 * @brief Immediately releases all memory and OS resources associated with a context.
 *
 * Destroys context mutexes, frees the memory arena, and releases all allocated trackers.
 *
 * @param ctx The workflow execution context to destroy.
 */
static void
cleanup_ctx_now(csilk_wf_ctx_t* ctx)
{
	csilk_mutex_destroy(&ctx->mutex);
	csilk_mutex_destroy(&ctx->arena_mutex);
	csilk_mutex_destroy(&ctx->trace_mutex);
	csilk_arena_free(ctx->arena);
	free(ctx->node_input_counts);
	free(ctx->node_approved);
	free(ctx->node_outputs);
	free(ctx->wal_path);
	free(ctx);
}

/**
 * @brief libuv handle close callback for the TTL timer.
 *
 * Triggers the final cleanup of the context after the libuv timer handle is closed.
 *
 * @param handle The csilk_io_handle_t pointer of the TTL timer.
 */
static void
on_ttl_timer_close(csilk_io_handle_t* handle)
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
CSILK_INTERNAL void
_wf_cleanup_ctx(csilk_wf_ctx_t* ctx)
{
	if (!ctx) {
		return;
	}
	unregister_active_ctx(ctx->wf, ctx);
	if (ctx->wf->ttl_sec > 0) {
		csilk_io_timer_stop(&ctx->ttl_timer);
		if (!csilk_io_is_closing((csilk_io_handle_t*)&ctx->ttl_timer)) {
			ctx->ttl_timer.data = ctx;
			csilk_io_close((csilk_io_handle_t*)&ctx->ttl_timer, on_ttl_timer_close);
			return;
		}
	}
	cleanup_ctx_now(ctx);
}

/* --- WAL Event Logging --- */

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

/* --- Node Execution --- */

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);
static void after_worker_cb(csilk_io_work_t* req, int status);

/** @brief libuv thread-pool work callback — executes a workflow node's
 *  handler on a background thread.
 *
 * Broadcasts a "node_start" event to monitors, then calls the node's
 * handler function. The output is stored in the work request struct
 * for retrieval by after_worker_cb on the main loop thread. */
static void
worker_cb(csilk_io_work_t* req)
{
	node_work_t* work = (node_work_t*)req->data;
	_wf_broadcast(work->ctx->wf, "node_start", work->node->id, nullptr);
	work->output = work->node->handler(work->ctx, work->input, work->node->user_data);
}

/**
 * @brief libuv timer callback invoked when a node retry delay expires.
 *
 * Re-queues the node's execution on the libuv thread pool.
 *
 * @param handle The libuv timer handle.
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
 * @brief libuv handle close callback for node work timer.
 *
 * Frees the node_work_t allocation after its timer handle is closed.
 *
 * @param handle The closed libuv handle.
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
	if (work->node->timeout_ms > 0 && !work->timer_closing) {
		work->timer_closing = 1;
		csilk_io_timer_stop(&work->node_timer);
		work->node_timer.data = work;
		csilk_io_close((csilk_io_handle_t*)&work->node_timer, on_work_timer_close);
		return;
	}
	free(work);
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
after_worker_cb(csilk_io_work_t* req, int status)
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
		CSILK_LOG_W("[Workflow] Node '%s' failed, scheduled retry in %dms",
			    node->id,
			    node->retry_delay_ms);

		if (node->retry_delay_ms > 0) {
			csilk_io_timer_init(ctx->wf->loop, &work->node_timer);
			work->node_timer.data = work;
			csilk_io_timer_start(
			    &work->node_timer, on_retry_timer, node->retry_delay_ms, 0);
			return; // Wait for timer
		} else {
			csilk_io_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
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
						CSILK_LOG_W("[Workflow] Node '%s' output failed "
							    "schema: missing required field '%s'",
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

	csilk_mutex_lock(&ctx->mutex);

	ctx->node_outputs[node->index] = output;
	if (output && output->meta) {
		csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
		ctx->total_tokens += am->prompt_tokens + am->completion_tokens;
	}
	csilk_mutex_unlock(&ctx->mutex);

	wal_log_event(ctx, WF_EV_NODE_FINISH, node->id, output);
	_wf_broadcast(ctx->wf, "node_finish", node->id, output ? (char*)output->value : nullptr);

	if (work->trace_node) {
		work->trace_node->end_time = csilk_io_hrtime() / 1000;
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
		csilk_mutex_lock(&ctx->trace_mutex);
		ctx->trace->nodes =
		    realloc(ctx->trace->nodes,
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
			if (ctx->trace_callback) {
				ctx->trace_callback(nullptr, ctx->trace);
			} else if (ctx->callback) {
				ctx->callback(nullptr);
			}
			if (ctx->trace_callback) {
				ctx->trace = nullptr;
			}
			_wf_cleanup_ctx(ctx);
		}
		free_work(work);
		return;
	}

	if (output == nullptr && node->error_target) {
		execute_node(ctx, node->error_target, nullptr);
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

			CSILK_LOG_D("[Workflow] Evaluating edge %zu to '%s' (match=%d)",
				    i,
				    edge->target->id,
				    match);

			if (match) {
				csilk_wf_node_t* target = edge->target;
				int ready = 0;
				csilk_mutex_lock(&ctx->mutex);
				if (ctx->total_executions < MAX_WORKFLOW_STEPS) {
					ctx->node_input_counts[target->index]++;
					int threshold = target->incoming_count == 0
							    ? 1
							    : target->incoming_count;

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
		wal_log_event(ctx, WF_EV_END, nullptr, nullptr);
		_wf_broadcast(
		    ctx->wf, "workflow_end", nullptr, output ? (char*)output->value : nullptr);
		if (ctx->trace) {
			ctx->trace->end_time = csilk_io_hrtime() / 1000;
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
		_wf_cleanup_ctx(ctx);
	}
	free_work(work);
}

/** @brief libuv timer callback — marks a node as timed out.
 *  Sets the is_timed_out flag on the node_work_t, which causes
 *  after_worker_cb to treat the output as nullptr even if the handler
 *  eventually completes. */
static void
on_node_timeout(csilk_io_timer_t* handle)
{
	node_work_t* work = (node_work_t*)handle->data;
	work->is_timed_out = 1;
	CSILK_LOG_W(
	    "[Workflow] Node '%s' timed out after %dms", work->node->id, work->node->timeout_ms);
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
 * 7. Queue the work via csilk_io_queue_work(). The node handler runs on a
 *    background thread; after_worker_cb processes the result.
 *
 * @param ctx   Workflow execution context.
 * @param node  The node to execute.
 * @param input Input data to pass to the node's handler. */
static void
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

		wal_log_event(ctx, WF_EV_PAUSE, node->id, input);
		_wf_broadcast(
		    ctx->wf, "workflow_paused", node->id, input ? (char*)input->value : nullptr);
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

		CSILK_LOG_I(
		    "[Workflow] Execution %s offloaded node '%s' to MQ", ctx->exec_id, node->id);

		free(json);
		cJSON_Delete(task);
		return;
	}
	csilk_mutex_unlock(&ctx->mutex);

	wal_log_event(ctx, WF_EV_NODE_START, node->id, nullptr);
	_wf_broadcast(ctx->wf, "node_queued", node->id, nullptr);

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

/* --- Public Run Entry Points --- */

/**
 * @brief Runs the workflow definition asynchronously.
 *
 * Instantiates a new execution context, generates a unique UUID, initializes
 * tracking arrays, triggers entry nodes (nodes with 0 incoming dependencies or explicitly marked),
 * and kicks off async execution on the libuv default loop.
 *
 * @param wf       The workflow definition instance.
 * @param input    The initial workflow input data.
 * @param callback Callback function invoked with the final workflow output when complete.
 * @return The unique execution ID string (UUID) assigned to this run. Do not free.
 */
const char*
csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input, void (*callback)(csilk_data_t* result))
{
	return _wf_run_ext_internal(wf, input, callback, nullptr);
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
csilk_wf_run_traced(csilk_wf_t* wf,
		    csilk_data_t* input,
		    void (*callback)(csilk_data_t* result, csilk_wf_trace_t* trace))
{
	_wf_run_ext_internal(wf, input, nullptr, callback);
}

/**
 * @brief libuv timer callback invoked when the global workflow TTL expires.
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
 * @return The assigned execution UUID, or nullptr on failure.
 */
const char*
_wf_run_ext_internal(csilk_wf_t* wf,
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
		ctx->trace->start_time = csilk_io_hrtime() / 1000;
	}
	int started = 0;
	static __thread char ret_exec_id[CSILK_UUID_BUF_SIZE];
	strcpy(ret_exec_id, ctx->exec_id);

	atomic_fetch_add(&ctx->nodes_active, 1);

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

	int active = atomic_fetch_sub(&ctx->nodes_active, 1) - 1;

	if (!started) {
		if (callback) {
			callback(nullptr);
		}
		if (trace_cb) {
			trace_cb(nullptr, nullptr);
		}
		if (active == 0) {
			_wf_cleanup_ctx(ctx);
		}
		return nullptr;
	}

	if (active == 0) {
		_wf_cleanup_ctx(ctx);
	}

	return ret_exec_id;
}

/* --- Resume & Signal --- */

/**
 * @brief Resumes an interrupted workflow execution from its Write-Ahead Log (WAL).
 *
 * Re-reads the WAL file to reconstruct the execution state (which nodes completed and
 * their outputs, which nodes were active/paused), reinstantiates the context, and
 * schedules remaining nodes to resume execution.
 *
 * @param wf       The workflow definition instance.
 * @param exec_id  The UUID of the workflow execution to recover and resume.
 * @param callback Callback function invoked with the final output when the resumed run completes.
 */
void
csilk_wf_resume(csilk_wf_t* wf, const char* exec_id, void (*callback)(csilk_data_t* result))
{
	if (!wf || !exec_id || !wf->wal_dir) {
		return;
	}

	cleanup_stale_ctx(wf, exec_id);

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
	csilk_mutex_init(&ctx->mutex);
	csilk_mutex_init(&ctx->arena_mutex);
	csilk_mutex_init(&ctx->trace_mutex);
	snprintf(ctx->exec_id, sizeof(ctx->exec_id), "%s", exec_id);
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
		_wf_cleanup_ctx(ctx);
	} else if (ctx->is_paused) {
		_wf_cleanup_ctx(ctx);
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

/**
 * @brief Signals a paused workflow context to resume execution.
 *
 * Used for interactive nodes that paused execution waiting for approval or human input.
 * Reconstructs state from the WAL, sets the paused node as approved, and resumes run.
 *
 * @param wf       The workflow definition instance.
 * @param exec_id  The unique execution ID (UUID) that is currently paused.
 * @param input    Optional new/edited input data to feed to the resuming node.
 * @param callback Callback function invoked when the workflow run eventually completes.
 */
void
csilk_wf_signal_continue(csilk_wf_t* wf,
			 const char* exec_id,
			 csilk_data_t* input,
			 void (*callback)(csilk_data_t* result))
{
	if (!wf || !exec_id || !wf->wal_dir) {
		return;
	}

	cleanup_stale_ctx(wf, exec_id);

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
	csilk_mutex_init(&ctx->mutex);
	csilk_mutex_init(&ctx->arena_mutex);
	csilk_mutex_init(&ctx->trace_mutex);
	snprintf(ctx->exec_id, sizeof(ctx->exec_id), "%s", exec_id);
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
		_wf_cleanup_ctx(ctx);
	}
}
