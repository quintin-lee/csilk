/**
 * @file recovery.c
 * @brief Panic recovery middleware implementation.
 * @copyright MIT License
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief Panic recovery middleware — catches longjmp panics and returns 500.
 *
 * Installs a setjmp recovery point before calling csilk_next(). If any
 * downstream handler triggers csilk_panic(), execution resumes at the
 * setjmp point and an "Internal Server Error" response (500) is sent
 * instead of crashing the process.
 *
 * After the normal (non-panic) path completes, the jump buffer flag is
 * cleared.
 *
 * @param c  The request context (must have a valid jump_buffer).
 *
 * @note This middleware MUST be registered before any handler that may
 *       call csilk_panic().
 * @warning setjmp/longjmp do NOT unwind the C stack or call destructors.
 *          Resources allocated by panicked handlers (malloc, open files,
 *          etc.) WILL leak. Use this as a last-resort safety net, not as
 *          a primary error-handling mechanism.
 */
void
csilk_recovery_handler(csilk_ctx_t* c)
{
	/* Install a recovery landing point. If csilk_panic() is called by any
     downstream handler, execution resumes here with setjmp returning
     non-zero (the value passed to longjmp). */
	if (setjmp(c->jump_buffer) == 0) {
		c->has_jump_buffer = 1;
		csilk_next(c);
		c->has_jump_buffer = 0; /* Normal path: clear the flag. */
	} else {
		/* Panic path: a downstream handler called csilk_panic().
       Send a generic 500 to avoid leaking internal state. */
		c->has_jump_buffer = 0;
		csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "Internal Server Error");
	}
}

/**
 * @brief Trigger a panic (longjmp to recovery handler) or abort if no recovery
 *        registered.
 *
 * If a recovery handler has been installed (c->has_jump_buffer is set), this
 * function performs a longjmp back to the recovery point set by
 * csilk_recovery_handler().
 *
 * If no recovery handler is registered, it prints a fatal error message to
 * stderr and calls exit(1) to terminate the process.
 *
 * @param c  The request context. May be NULL (will trigger abort path).
 *
 * @warning This function does NOT return when executed without a recovery
 *          handler.
 */
void
csilk_panic(csilk_ctx_t* c)
{
	if (c->has_jump_buffer) {
		longjmp(c->jump_buffer, 1);
	} else {
		fprintf(stderr, "Fatal: No recovery handler registered.\n");
		exit(1);
	}
}
