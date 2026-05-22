#include <stdio.h>
#include <time.h>

#include "../include/gin.h"

void gin_logger_handler(gin_ctx_t* c) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  gin_next(c);

  clock_gettime(CLOCK_MONOTONIC, &end);
  double duration =
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

  // Assuming gin_ctx_t has method, path, and response status
  printf("[GIN] %s %s %d %.6f s\n", c->request.method, c->request.path,
         c->response.status, duration);
}
