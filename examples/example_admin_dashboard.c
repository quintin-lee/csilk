/**
 * @file example_admin_dashboard.c
 * @brief Comprehensive demonstration of the unified administration and
 * monitoring dashboard.
 *
 * This example showcases:
 * - Real-time HTTP metrics tracking (via metrics middleware).
 * - Multi-core multi-worker architecture.
 * - Database integration (SQLite) with telemetry.
 * - Internal Message Queue (MQ) activity.
 * - Liveness (/healthz) and Readiness (/readyz) probes.
 * - Unified Admin Dashboard (/admin) for full-spectrum observability.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/app/admin.h"
#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/* Internal framework function for demonstration purposes only. */
extern void _csilk_metrics_inc_auth_failures(void);

/* Global DB pool for the example */
static csilk_db_pool_t* global_db_pool = NULL;

/* --- Handlers --- */

/** @brief Simple hello handler to increment HTTP request counters. */
void hello_handler(csilk_ctx_t* c) {
  csilk_string(c, 200, "Hello from Csilk! Check /admin for real-time stats.");
}

/** @brief Simulates a slow request to show latency distribution in dashboard.
 */
void slow_handler(csilk_ctx_t* c) {
  /* Simulate work/latency */
  usleep(250000);  // 250ms
  csilk_string(c, 200, "Response delayed by 250ms.");
}

/** @brief Publishes a message to the internal event bus. */
void mq_handler(csilk_ctx_t* c) {
  csilk_mq_t* mq = csilk_server_get_mq(csilk_ctx_get_server(c));
  const char* msg = "Manual trigger event";

  /* Publishes to the MQ; stats will be updated in real-time on dashboard. */
  csilk_mq_publish(mq, "user.activity", msg, strlen(msg));
  csilk_string(c, 200, "Event published to internal MQ topic 'user.activity'.");
}

/** @brief Performs a database query to generate DB telemetry. */
void db_handler(csilk_ctx_t* c) {
  /* Retrieve the database pool. */
  if (!global_db_pool) {
    csilk_string(c, 500, "Database pool not initialized.");
    return;
  }

  /* Execute a simple query; telemetry is automatically collected by the driver.
   */
  cJSON* result =
      csilk_db_query_json(global_db_pool, "SELECT * FROM users LIMIT 1");
  if (result) {
    char* json_str = cJSON_PrintUnformatted(result);
    csilk_string(c, 200, json_str);
    free(json_str);
    cJSON_Delete(result);
  } else {
    csilk_string(c, 500, "Database query failed.");
  }
}

/** @brief Simulates an authentication failure for security stats. */
void auth_fail_handler(csilk_ctx_t* c) {
  /* Manually increment the auth failure metric for demonstration. */
  _csilk_metrics_inc_auth_failures();
  csilk_string(c, 401, "Simulated unauthorized access.");
}

/* --- Background Tasks --- */

/** @brief Background timer callback to generate persistent MQ traffic. */
static void generate_bg_traffic(uv_timer_t* h) {
  csilk_mq_t* mq = (csilk_mq_t*)h->data;
  const char* tick_msg = "system_heartbeat";
  csilk_mq_publish(mq, "system.metrics", tick_msg, strlen(tick_msg));
}

/* --- Main Application --- */

int main() {
  printf("====================================================\n");
  printf("  Csilk Advanced Admin Dashboard Example\n");
  printf("====================================================\n");
  printf("Endpoints:\n");
  printf(" - Dashboard:  http://localhost:8080/admin\n");
  printf(" - Metrics:    http://localhost:8080/metrics\n");
  printf(" - Liveness:   http://localhost:8080/healthz\n");
  printf(" - Readiness:  http://localhost:8080/readyz\n");
  printf(" - Generate MQ: http://localhost:8080/pub\n");
  printf(" - Generate DB: http://localhost:8080/db\n");
  printf("====================================================\n\n");

  /* 1. Create a high-level application wrapper. */
  csilk_app_t* app = csilk_app_new(NULL);

  /* 2. Configure Database (SQLite) */
  csilk_db_init(); /* Register built-in drivers */
  csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "example_admin.db");
  if (pool) {
    /* Initialize schema */
    csilk_db_exec(
        pool,
        "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)");
    csilk_db_exec(pool,
                  "INSERT INTO users (name) VALUES ('Csilk Dashboard Demo')");

    global_db_pool = pool;
    printf("[Init] SQLite pool ready at 'example_admin.db'\n");
  }

  /* 3. Global Middleware Setup */
  /* Enable the advanced metrics middleware globally to track all requests. */
  csilk_app_use(app, (csilk_handler_t)csilk_metrics_middleware);

  /* 4. Monitoring & Observability Integration */
  /* Standard Prometheus /metrics endpoint. */
  csilk_app_get(app, "/metrics", csilk_metrics_handler);

  /* Liveness and Readiness probes. */
  csilk_app_get(app, "/healthz", csilk_health_check_handler);
  csilk_app_get(app, "/readyz", csilk_ready_check_handler);

  /* Mount the Admin Dashboard under /admin.
     This handles the UI HTML, the /admin/stats JSON API, and /admin/ws
     real-time feed. */
  csilk_admin_serve(app, "/admin");

  /* 5. Business Logic Routes */
  csilk_app_get(app, "/", hello_handler);
  csilk_app_get(app, "/slow", slow_handler);
  csilk_app_get(app, "/pub", mq_handler);
  csilk_app_get(app, "/db", db_handler);
  csilk_app_get(app, "/secure-fail", auth_fail_handler);

  /* 6. Background Activity Setup */
  /* We use the underlying libuv event loop to schedule background tasks. */
  uv_timer_t mq_timer;
  uv_timer_init(uv_default_loop(), &mq_timer);
  mq_timer.data = csilk_server_get_mq(csilk_app_server(app));
  /* Start a timer to publish a message every 5 seconds. */
  uv_timer_start(&mq_timer, generate_bg_traffic, 1000, 5000);

  /* 7. Multi-Core Utilization (Optional configuration) */
  csilk_server_config_t cfg = {0};
  cfg.worker_threads =
      4; /* Utilize 4 CPU cores via SO_REUSEPORT architecture. */
  csilk_server_set_config(csilk_app_server(app), &cfg);

  /* 8. Execute Event Loop */
  csilk_app_run(app, 8080);

  /* Cleanup (executed on graceful shutdown) */
  if (pool) csilk_db_pool_free(pool);
  csilk_app_free(app);

  return 0;
}
