/**
 * @file logger.c
 * @brief Logging middleware implementation.
 * MIT License
 */

#include <stdio.h>
#include <time.h>

#include "csilk.h"

/** @brief Logging middleware handler.
 * @param c The request context. */
void csilk_logger_handler(csilk_ctx_t* c) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  csilk_next(c);

  clock_gettime(CLOCK_MONOTONIC, &end);
  double duration =
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

  CSILK_LOG_I("[HTTP] %s %s %d %.6f s", 
         c->request.method ? c->request.method : "UNKNOWN", 
         c->request.path ? c->request.path : "UNKNOWN",
         c->response.status, duration);
}
