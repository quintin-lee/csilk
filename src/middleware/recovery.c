/**
 * @file recovery.c
 * @brief Panic recovery middleware implementation.
 * @copyright MIT License
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Panic recovery middleware — catches longjmp panics and returns 500.
 */
void csilk_recovery_handler(csilk_ctx_t* c) {
  if (setjmp(c->jump_buffer) == 0) {
    c->has_jump_buffer = 1;
    csilk_next(c);
    c->has_jump_buffer = 0;  // Reset after normal flow
  } else {
    // Panic occurred, send 500
    c->has_jump_buffer = 0;  // Ensure reset on panic path too
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR,
                 "Internal Server Error");
  }
}

/** @brief Trigger a panic (longjmp to recovery handler) or abort if no recovery
 * registered. */
void csilk_panic(csilk_ctx_t* c) {
  if (c->has_jump_buffer) {
    longjmp(c->jump_buffer, 1);
  } else {
    fprintf(stderr, "Fatal: No recovery handler registered.\n");
    exit(1);
  }
}
