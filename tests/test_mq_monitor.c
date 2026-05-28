#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "cJSON.h"
#include "csilk/csilk.h"

static int g_done = 0;

void dummy_mq_handler(csilk_mq_ctx_t* ctx) {
  (void)ctx;
  csilk_mq_next(ctx);
}

void test_mq_monitoring_logic() {
  printf("Testing MQ status monitoring and stats...\n");

  csilk_server_t* server = csilk_server_new(NULL);
  csilk_mq_t* mq = csilk_server_get_mq(server);

  // 1. Subscribe and Publish
  csilk_mq_subscribe(mq, "test.topic", dummy_mq_handler);
  csilk_mq_publish(mq, "test.topic", "hello", 5);
  csilk_mq_publish(mq, "test.topic", "world", 5);

  // 2. Register monitor smoke test
  csilk_ctx_t* mock_ctx = calloc(1, 1024);
  csilk_mq_register_monitor(mq, mock_ctx);

  // 3. Run loop to process messages
  for (int i = 0; i < 10; i++) {
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    usleep(1000);
  }

  // 4. Verify stats
  csilk_mq_stats_t stats;
  csilk_mq_get_stats(mq, &stats);

  printf("[Test] Stats: Pub=%lu, Del=%lu, Depth=%u, Topics=%u\n",
         stats.published_total, stats.delivered_total, stats.queue_depth,
         stats.topic_count);

  assert(stats.published_total == 2);
  assert(stats.delivered_total == 2);
  assert(stats.queue_depth == 0);
  assert(stats.topic_count == 1);

  // 5. Verify JSON conversion
  char* json = csilk_mq_stats_to_json(&stats);
  assert(json != NULL);
  printf("[Test] Stats JSON: %s\n", json);

  cJSON* root = cJSON_Parse(json);
  assert(root != NULL);
  assert(cJSON_GetObjectItem(root, "published_total")->valuedouble == 2);
  assert(cJSON_GetObjectItem(root, "delivered_total")->valuedouble == 2);

  cJSON_Delete(root);
  free(json);
  csilk_server_free(server);
  free(mock_ctx);
  printf("test_mq_monitoring_logic: PASS\n");
}

int main() {
  test_mq_monitoring_logic();
  return 0;
}
