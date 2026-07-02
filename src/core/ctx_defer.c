/**
 * @file ctx_defer.c
 * @brief Panic-safe deferred resource cleanup for csilk context.
 *
 * Deferred cleanups are registered during normal handler execution and
 * automatically invoked when csilk_panic() triggers a longjmp back to
 * the recovery handler.  Because longjmp does NOT unwind the C stack,
 * deferred cleanups are the only way to safely release resources across
 * a panic boundary.
 *
 * @copyright MIT License
 */

#include "core/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Register a cleanup function to run on panic.
 *
 * The function will be executed by csilk_ctx_defer_free() when recovery
 * catches a panic.  Allocated in arena memory so it is cleaned up with
 * the request context after the response is sent.
 *
 * @param c   The request context.
 * @param fn  Cleanup function (e.g. close an fd, release a mutex).
 * @param arg User data passed to @p fn.
 * @return 0 if registered, -1 if context/arena/fn is invalid. */
int
csilk_ctx_defer(csilk_ctx_t* c, void (*fn)(void*), void* arg)
{
    if (!c || !fn || !c->arena) {
        return -1;
    }

    csilk_defer_item_t* item = csilk_arena_alloc(c->arena, sizeof(csilk_defer_item_t));
    if (!item) {
        return -1;
    }

    item->fn = fn;
    item->arg = arg;
    item->next = c->defer_head;
    c->defer_head = item;

    return 0;
}

/** @brief Execute all registered deferred cleanups in reverse order.
 *
 * Called by the recovery handler after catching a panic.  Walks the
 * deferred item linked list (LIFO — most recently registered first) and
 * invokes each cleanup function.  The list is cleared by this call.
 *
 * @param c The request context. */
void
csilk_ctx_defer_free(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    csilk_defer_item_t* item = c->defer_head;
    while (item) {
        if (item->fn) {
            item->fn(item->arg);
        }
        item = item->next;
    }
    c->defer_head = nullptr;
}
