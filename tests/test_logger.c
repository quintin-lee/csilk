#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#include "gin.h"

// Mock handler
void dummy_handler(gin_ctx_t* c) { c->response.status = 200; }

#define NUM_THREADS 5
#define LOGS_PER_THREAD 10

void* thread_func(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        GIN_LOG_I("Thread %d: log message %d", id, i);
        usleep(1000); // 1ms
    }
    return NULL;
}

int main() {
  printf("Initializing logger...\n");
  gin_log_config_t cfg = {
      .level = GIN_LOG_DEBUG,
      .file_path = NULL,
      .max_file_size = 0,
      .use_colors = -1
  };
  assert(gin_log_init(cfg) == 0);

  // 1. Test basic middleware logging
  gin_ctx_t c = {0};
  c.request.method = "GET";
  c.request.path = "/test";
  gin_handler_t handlers[] = {gin_logger_handler, dummy_handler, NULL};
  c.handlers = handlers;
  c.handler_index = -1; // gin_next increments this

  printf("Testing middleware logging...\n");
  gin_next(&c);
  assert(c.response.status == 200);

  // 2. Test multi-threaded logging
  printf("Testing multi-threaded logging...\n");
  pthread_t threads[NUM_THREADS];
  int thread_ids[NUM_THREADS];

  for (int i = 0; i < NUM_THREADS; i++) {
      thread_ids[i] = i;
      pthread_create(&threads[i], NULL, thread_func, &thread_ids[i]);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
      pthread_join(threads[i], NULL);
  }

  // 3. Test logging to file & rotation
  printf("Testing file logging & rotation...\n");
  gin_log_close(); 
  const char* log_file = "test.log";
  
  gin_log_config_t rot_cfg = {
      .level = GIN_LOG_INFO,
      .file_path = log_file,
      .max_file_size = 100, // Very small for rotation test
      .use_colors = 0
  };
  assert(gin_log_init(rot_cfg) == 0);
  
  GIN_LOG_I("Message 1: This is a test message to trigger rotation.");
  GIN_LOG_I("Message 2: This should be in a rotated file if size exceeded.");
  
  gin_log_close();

  // Verify file existence
  struct stat st;
  assert(stat(log_file, &st) == 0);
  assert(stat("test.log.1", &st) == 0); // Rotation should have happened

  remove(log_file);
  remove("test.log.1");

  printf("test_logger: PASS\n");
  return 0;
}
