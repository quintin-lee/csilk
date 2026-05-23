/**
 * @file logger.c
 * @brief Logging middleware implementation.
 * MIT License
 */

#include <stdio.h>
#include <time.h>

#include "gin.h"

/** @brief Logging middleware handler.
 * @param c The request context. */
void gin_logger_handler(gin_ctx_t* c) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  gin_next(c);

  clock_gettime(CLOCK_MONOTONIC, &end);
  double duration =
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

  GIN_LOG_I("[HTTP] %s %s %d %.6f s", 
         c->request.method ? c->request.method : "UNKNOWN", 
         c->request.path ? c->request.path : "UNKNOWN",
         c->response.status, duration);
}
