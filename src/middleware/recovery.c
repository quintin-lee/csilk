/**
 * @file recovery.c
 * @brief Recovery middleware implementation.
 * @license MIT
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "gin.h"

/** @brief Recovery middleware handler.
 * @param c The request context. */
void gin_recovery_handler(gin_ctx_t* c) {
  if (setjmp(c->jump_buffer) == 0) {
    c->has_jump_buffer = 1;
    gin_next(c);
    c->has_jump_buffer = 0; // Reset after normal flow
  } else {
    // Panic occurred, send 500
    c->has_jump_buffer = 0; // Ensure reset on panic path too
    gin_string(c, 500, "Internal Server Error");
  }
}

/** @brief Panic in the handler.
 * @param c The request context. */
void gin_panic(gin_ctx_t* c) {
  if (c->has_jump_buffer) {
    longjmp(c->jump_buffer, 1);
  } else {
    fprintf(stderr, "Fatal: No recovery handler registered.\n");
    exit(1);
  }
}
