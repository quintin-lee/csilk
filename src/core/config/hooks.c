/**
 * @file hooks.c
 * @brief Thread-safe RCU & Copy-On-Write lifecycle hook system for csilk.
 *
 * Design:
 *   - Readers (Hot Path): Lock-free, zero-allocation O(N) linear array traversal
 *     over contiguous memory (csilk_hook_array_t).
 *   - Writers (Registration/Removal): Copy-On-Write (CoW) guarded by s->hook_mutex,
 *     atomic pointer publication (memory_order_release), and EBR/RCU grace-period
 *     synchronization before reclaiming retired arrays.
 *
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/hooks.h"
#include "csilk/core/server.h"
#include "../internal/srv_internal.h"

void csilk_server_wait_grace_period(csilk_server_t* server);

/** @brief Register a lifecycle hook on the server with Copy-On-Write semantics.
 *
 * Hooks are invoked at specific points in the request lifecycle
 * (conn_open, conn_close, request_begin, request_end, server_start,
 * server_stop). Multiple handlers can be registered for the same hook type;
 * they are called in reverse order of registration (LIFO).
 *
 * @param s       The server instance.
 * @param type    Hook type (CSILK_HOOK_SERVER_START through CSILK_HOOK_REQUEST_END).
 * @param handler Function pointer matching the hook signature. */
void
csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler)
{
    if (!s || (unsigned)type >= CSILK_HOOK_COUNT || !handler) {
        return;
    }

    csilk_mutex_lock(&s->hook_mutex);

    csilk_hook_array_t* old_arr = atomic_load_explicit(&s->hooks[type], memory_order_relaxed);
    size_t              old_count = old_arr ? old_arr->count : 0;
    size_t              new_count = old_count + 1;

    csilk_hook_array_t* new_arr = malloc(sizeof(csilk_hook_array_t) + new_count * sizeof(void*));
    if (!new_arr) {
        csilk_mutex_unlock(&s->hook_mutex);
        return;
    }

    new_arr->count = new_count;
    /* LIFO: New handler goes to index 0 */
    new_arr->handlers[0] = handler;
    if (old_count > 0 && old_arr) {
        memcpy(&new_arr->handlers[1], old_arr->handlers, old_count * sizeof(void*));
    }

    /* Atomically publish the new immutable array */
    atomic_store_explicit(&s->hooks[type], new_arr, memory_order_release);

    /* Reclaim old array after ensuring no concurrent reader is reading it */
    if (old_arr) {
        atomic_fetch_add_explicit(&s->reload_mgr.global_epoch, 1, memory_order_acq_rel);
        csilk_server_wait_grace_period(s);
        free(old_arr);
    }

    csilk_mutex_unlock(&s->hook_mutex);
}

/** @brief Remove a previously registered hook handler using Copy-On-Write and RCU.
 *
 * @param s       The server instance.
 * @param type    The hook type.
 * @param handler Pointer to the callback function to remove.
 * @return 0 if found and removed, -1 if not found. */
int
csilk_server_remove_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler)
{
    if (!s || (unsigned)type >= CSILK_HOOK_COUNT || !handler) {
        return -1;
    }

    csilk_mutex_lock(&s->hook_mutex);

    csilk_hook_array_t* old_arr = atomic_load_explicit(&s->hooks[type], memory_order_relaxed);
    if (!old_arr || old_arr->count == 0) {
        csilk_mutex_unlock(&s->hook_mutex);
        return -1;
    }

    /* Find target handler */
    size_t found_idx = (size_t)-1;
    for (size_t i = 0; i < old_arr->count; i++) {
        if (old_arr->handlers[i] == handler) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == (size_t)-1) {
        csilk_mutex_unlock(&s->hook_mutex);
        return -1;
    }

    if (old_arr->count == 1) {
        atomic_store_explicit(&s->hooks[type], NULL, memory_order_release);
        atomic_fetch_add_explicit(&s->reload_mgr.global_epoch, 1, memory_order_acq_rel);
        csilk_server_wait_grace_period(s);
        free(old_arr);
        csilk_mutex_unlock(&s->hook_mutex);
        return 0;
    }

    size_t              new_count = old_arr->count - 1;
    csilk_hook_array_t* new_arr = malloc(sizeof(csilk_hook_array_t) + new_count * sizeof(void*));
    if (!new_arr) {
        csilk_mutex_unlock(&s->hook_mutex);
        return -1;
    }

    new_arr->count = new_count;
    size_t dst = 0;
    for (size_t i = 0; i < old_arr->count; i++) {
        if (i == found_idx) {
            continue;
        }
        new_arr->handlers[dst++] = old_arr->handlers[i];
    }

    atomic_store_explicit(&s->hooks[type], new_arr, memory_order_release);
    atomic_fetch_add_explicit(&s->reload_mgr.global_epoch, 1, memory_order_acq_rel);
    csilk_server_wait_grace_period(s);
    free(old_arr);

    csilk_mutex_unlock(&s->hook_mutex);
    return 0;
}

/** @brief Clear all registered hooks for a given hook type.
 *
 * @param s    The server instance.
 * @param type The hook type to clear. */
void
csilk_server_clear_hooks(csilk_server_t* s, csilk_hook_type_t type)
{
    if (!s || (unsigned)type >= CSILK_HOOK_COUNT) {
        return;
    }

    csilk_mutex_lock(&s->hook_mutex);

    csilk_hook_array_t* old_arr = atomic_load_explicit(&s->hooks[type], memory_order_relaxed);
    if (old_arr) {
        atomic_store_explicit(&s->hooks[type], NULL, memory_order_release);
        atomic_fetch_add_explicit(&s->reload_mgr.global_epoch, 1, memory_order_acq_rel);
        csilk_server_wait_grace_period(s);
        free(old_arr);
    }

    csilk_mutex_unlock(&s->hook_mutex);
}

/** @brief Internal: invoke all registered handlers for a given hook type.
 *
 * Hot path: Lock-free O(N) sequential array execution over contiguous memory.
 *
 * @param s    The server instance.
 * @param c    The request context (may be NULL for server-level hooks).
 * @param type Hook type to trigger. */
CSILK_INTERNAL void
_csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type)
{
    if (!s || (unsigned)type >= CSILK_HOOK_COUNT) {
        return;
    }

    csilk_hook_array_t* arr = atomic_load_explicit(&s->hooks[type], memory_order_acquire);
    if (!arr || arr->count == 0) {
        return;
    }

    size_t count = arr->count;
    void** handlers = arr->handlers;

    if (type <= CSILK_HOOK_SERVER_STOP) {
        for (size_t i = 0; i < count; i++) {
            csilk_server_hook_handler_t handler = NULL;
            memcpy(&handler, &handlers[i], sizeof(handler));
            handler(s);
        }
    } else if (c) {
        for (size_t i = 0; i < count; i++) {
            csilk_ctx_hook_handler_t handler = NULL;
            memcpy(&handler, &handlers[i], sizeof(handler));
            handler(c);
        }
    }
}
