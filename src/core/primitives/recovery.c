/**
 * @file recovery.c
 * @brief Panic recovery middleware implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief Panic recovery middleware — catches panics and returns 500.
 *
 * Calls csilk_next(). If any downstream handler triggers csilk_panic(),
 * the context is marked as panicked, and this middleware performs deferred
 * cleanups and returns a 500 status.
 *
 * @param c  The request context.
 */
void
csilk_recovery_handler(csilk_ctx_t* c)
{
    CSILK_LOG_T("Recovery: executing handler chain");
    csilk_next(c);
    if (c->panicked) {
        /* Panic path: a downstream handler called csilk_panic().
           Execute deferred cleanups to release heap memory, file
           descriptors, and mutexes, then send a generic 500. */
        CSILK_LOG_W("Recovery: panic recovered in request handler! Executing deferred cleanups.");
        csilk_ctx_defer_free(c);
        csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "Internal Server Error");
    }
}

/**
 * @brief Trigger a panic (safely aborts context and sets panicked flag).
 *
 * Marks the context as panicked and aborted, stops the handler chain execution,
 * and triggers the recovery cleanup path.
 *
 * @param c  The request context.
 */
void
csilk_panic(csilk_ctx_t* c)
{
    if (!c) {
        CSILK_LOG_F("Recovery: fatal - csilk_panic called with null context.");
        exit(1);
    }
    CSILK_LOG_W("Recovery: triggering panic (safe error propagation)");
    c->panicked = 1;
    c->aborted = 1;
    csilk_ctx_defer_free(c);
}
