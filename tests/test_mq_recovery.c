#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"

int received_count = 0;

void on_msg(csilk_mq_ctx_t* ctx) {
  received_count++;
  csilk_mq_next(ctx);
}

void test_mq_recovery() {
  printf("Testing MQ Recovery...\n");
  const char* wal_path = "test_recovery.wal";
  unlink(wal_path);

  {
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    csilk_mq_t* mq = csilk_server_get_mq(server);

    int rc = csilk_mq_set_persistence(mq, wal_path);
    assert(rc == 0);
    csilk_mq_publish(mq, "topic1", "msg1", 5);
    csilk_mq_publish(mq, "topic2", "msg2", 5);

    csilk_server_free(server);
    csilk_router_free(router);

    /* Process close callbacks */
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  }

  printf("Server restarted, recovering...\n");
  received_count = 0;
  {
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    csilk_mq_t* mq = csilk_server_get_mq(server);

    csilk_mq_subscribe(mq, "topic1", on_msg);
    csilk_mq_subscribe(mq, "topic2", on_msg);

    /* This should trigger recovery */
    int rc = csilk_mq_set_persistence(mq, wal_path);
    assert(rc == 0);

    /* Process messages (they are queued in async handle) */
    uv_loop_t* loop = uv_default_loop();
    /* Need to run the loop to trigger the async callback */
    uv_run(loop, UV_RUN_NOWAIT);
    uv_run(loop, UV_RUN_NOWAIT);

    if (received_count != 2) {
      printf("Error: received_count is %d, expected 2\n", received_count);
      /* Do not unlink so we can debug if needed */
      exit(1);
    }

    csilk_server_free(server);
    csilk_router_free(router);
    uv_run(loop, UV_RUN_NOWAIT);
  }

  unlink(wal_path);
  printf("test_mq_recovery: PASS\n");
}

int main() {
  test_mq_recovery();
  return 0;
}
