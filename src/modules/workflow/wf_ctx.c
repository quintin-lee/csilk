/**
 * @file wf_ctx.c
 * @brief Workflow context lifecycle: registration, lookup, cleanup.
 */

#include <string.h>
#include <stdlib.h>

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
void
register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
    csilk_mutex_lock(&wf->ctx_mutex);
    if (wf->active_context_count >= wf->active_context_capacity) {
        size_t new_cap = wf->active_context_capacity == 0 ? 8 : wf->active_context_capacity * 2;
        csilk_wf_ctx_t** new_ctxs = realloc(wf->active_contexts, sizeof(csilk_wf_ctx_t*) * new_cap);
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
 * @return A pointer to the matching csilk_wf_ctx_t, or NULL if not found.
 */
static csilk_wf_ctx_t*
find_active_ctx(csilk_wf_t* wf, const char* exec_id)
{
    csilk_wf_ctx_t* found = NULL;
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

/**
 * @brief Terminates and reclaims any stale active context for an exec_id.
 *
 * Looks up the active context by exec_id; if found, marks it terminated and,
 * when no nodes are still running, cleans it up immediately, otherwise just
 * unregisters it so in-flight nodes free it once drained.
 *
 * @param wf      The workflow definition instance.
 * @param exec_id The execution ID whose stale context should be reclaimed.
 */
void
_wf_cleanup_stale_ctx(csilk_wf_t* wf, const char* exec_id)
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
 * @return A pointer to the active context, or NULL if not found.
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
 * @brief Handle close callback for the TTL timer.
 *
 * Triggers the final cleanup of the context after the timer handle is closed.
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
 * @param ctx The execution context to clean up (may be NULL). */
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
