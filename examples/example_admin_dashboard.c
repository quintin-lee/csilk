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

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/app/admin.h"
#include "csilk/app/app.h"
#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/* Internal framework functions for demonstration purposes only. */
extern void _csilk_metrics_inc_auth_failures(void);
extern void _csilk_metrics_inc_rate_limit_blocks(void);
extern void _csilk_metrics_inc_csrf_violations(void);

/* Global DB pool for the example */
static csilk_db_pool_t* global_db_pool = nullptr;

/* --- Handlers --- */

void
hello_handler(csilk_ctx_t* c)
{
	csilk_string(c, 200, "Hello from Csilk! Check /admin for real-time stats.");
}

void
slow_handler(csilk_ctx_t* c)
{
	usleep(250000);
	csilk_string(c, 200, "Response delayed by 250ms.");
}

void
mq_handler(csilk_ctx_t* c)
{
	csilk_mq_t* mq = csilk_server_get_mq(csilk_ctx_get_server(c));
	const char* msg = "Manual trigger event";
	csilk_mq_publish(mq, "user.activity", msg, strlen(msg));
	csilk_string(c, 200, "Event published to internal MQ topic 'user.activity'.");
}

void
db_handler(csilk_ctx_t* c)
{
	if (!global_db_pool) {
		csilk_string(c, 500, "Database pool not initialized.");
		return;
	}
	cJSON* result = csilk_db_query_json(global_db_pool, "SELECT * FROM users LIMIT 1");
	if (result) {
		char* json_str = cJSON_PrintUnformatted(result);
		csilk_string(c, 200, json_str);
		free(json_str);
		cJSON_Delete(result);
	} else {
		csilk_string(c, 500, "Database query failed.");
	}
}

void
auth_fail_handler(csilk_ctx_t* c)
{
	_csilk_metrics_inc_auth_failures();
	csilk_string(c, 401, "Simulated unauthorized access.");
}

/* --- Mock AI Driver --- */

typedef struct {
	char _unused;
} mock_ai_state_t;

static void*
mock_ai_init(const char* api_key, const char* base_url)
{
	(void)api_key;
	(void)base_url;
	return calloc(1, sizeof(mock_ai_state_t));
}

static int
mock_ai_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
	(void)state;
	(void)req;
	if (!res) {
		return -1;
	}
	res->content = strdup("Mock response for dashboard AI demo.");
	res->prompt_tokens = 12;
	res->completion_tokens = 28;
	res->total_tokens = 40;
	return 0;
}

static int
mock_ai_embeddings(void* state,
		   const char* model,
		   const char** input,
		   size_t count,
		   csilk_ai_embeddings_response_t* res)
{
	(void)state;
	(void)model;
	(void)input;
	(void)count;
	if (!res) {
		return -1;
	}
	res->values = calloc(4, sizeof(float));
	res->values[0] = 0.1f;
	res->values[1] = 0.2f;
	res->values[2] = 0.3f;
	res->values[3] = 0.4f;
	res->dimension = 4;
	res->count = 1;
	res->total_tokens = 8;
	return 0;
}

static void
mock_ai_free(void* state)
{
	free(state);
}

static const csilk_ai_driver_t mock_ai_driver = {
    .name = "mock",
    .init = mock_ai_init,
    .chat = mock_ai_chat,
    .embeddings = mock_ai_embeddings,
    .free = mock_ai_free,
};

static csilk_ai_t* g_mock_ai = nullptr;

/* --- Background Tasks --- */

static void
generate_bg_traffic(uv_timer_t* h)
{
	csilk_mq_t* mq = (csilk_mq_t*)h->data;
	csilk_mq_publish(mq, "system.heartbeat", "tick", 4);
}

static void
generate_db_traffic(uv_timer_t* h)
{
	(void)h;
	if (!global_db_pool) {
		return;
	}
	csilk_db_exec(global_db_pool, "INSERT OR IGNORE INTO users (name) VALUES ('bg_demo_user')");
	cJSON* result = csilk_db_query_json(global_db_pool, "SELECT COUNT(*) AS cnt FROM users");
	if (result) {
		cJSON_Delete(result);
	}
}

static void
generate_ai_traffic(uv_timer_t* h)
{
	(void)h;
	if (!g_mock_ai) {
		return;
	}
	csilk_ai_message_t msgs[] = {{(char*)"user", (char*)"Hello from dashboard demo"}};
	csilk_ai_chat_request_t req = {.model = "mock-model",
				       .messages = msgs,
				       .message_count = 1,
				       .max_tokens = 100,
				       .temperature = 0.7};
	csilk_ai_chat_response_t res = {0};
	csilk_ai_chat(g_mock_ai, &req, &res);
	csilk_ai_chat_response_free(&res);
}

/* --- Background HTTP Traffic --- */

struct http_batch {
	const char* urls[8];
	int count;
};

static size_t
curl_write_null(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	(void)ptr;
	(void)userdata;
	return size * nmemb;
}

static void
http_work_cb(uv_work_t* req)
{
	struct http_batch* batch = (struct http_batch*)req->data;
	CURL* curl = curl_easy_init();
	if (!curl) {
		return;
	}
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_null);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	for (int i = 0; i < batch->count; i++) {
		curl_easy_setopt(curl, CURLOPT_URL, batch->urls[i]);
		curl_easy_perform(curl);
	}
	curl_easy_cleanup(curl);
}

static void
http_after_cb(uv_work_t* req, int status)
{
	(void)status;
	free(req->data);
	free(req);
}

static void
generate_http_traffic(uv_timer_t* h)
{
	(void)h;
	uv_work_t* work = malloc(sizeof(uv_work_t));
	if (!work) {
		return;
	}
	struct http_batch* batch = malloc(sizeof(struct http_batch));
	if (!batch) {
		free(work);
		return;
	}
	batch->urls[0] = "http://localhost:8080/";
	batch->urls[1] = "http://localhost:8080/slow";
	batch->urls[2] = "http://localhost:8080/secure-fail";
	batch->urls[3] = "http://localhost:8080/db";
	batch->urls[4] = "http://localhost:8080/pub";
	batch->count = 5;
	work->data = batch;
	uv_queue_work(csilk_io_default_loop(), work, http_work_cb, http_after_cb);
}

static void
generate_security_traffic(uv_timer_t* h)
{
	(void)h;
	_csilk_metrics_inc_rate_limit_blocks();
	_csilk_metrics_inc_csrf_violations();
}

/* --- Main Application --- */

int
main()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	printf("====================================================\n");
	printf("  Csilk Advanced Admin Dashboard Example\n");
	printf("====================================================\n");
	printf("Endpoints:\n");
	printf(" - Dashboard:  http://localhost:8080/admin\n");
	printf(" - Metrics:    http://localhost:8080/metrics\n");
	printf(" - Liveness:   http://localhost:8080/healthz\n");
	printf(" - Readiness:  http://localhost:8080/readyz\n");
	printf("====================================================\n\n");

	csilk_app_t* app = csilk_app_new(nullptr);

	/* Database (SQLite) */
	csilk_db_init();
	csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "example_admin.db");
	if (pool) {
		csilk_db_exec(pool,
			      "CREATE TABLE IF NOT EXISTS users (id INTEGER "
			      "PRIMARY KEY, name TEXT)");
		csilk_db_exec(pool,
			      "INSERT OR IGNORE INTO users (name) VALUES "
			      "('Dashboard Demo User')");
		global_db_pool = pool;
		printf("[Init] SQLite pool ready at 'example_admin.db'\n");
	}

	/* Global Middleware */
	csilk_app_use(app, (csilk_handler_t)csilk_metrics_middleware);

	/* Monitoring & Observability */
	csilk_app_get(app, "/metrics", csilk_metrics_handler);
	csilk_app_get(app, "/healthz", csilk_health_check_handler);
	csilk_app_get(app, "/readyz", csilk_ready_check_handler);
	csilk_admin_serve(app, "/admin");

	/* Business Logic Routes */
	csilk_app_get(app, "/", hello_handler);
	csilk_app_get(app, "/slow", slow_handler);
	csilk_app_get(app, "/pub", mq_handler);
	csilk_app_get(app, "/db", db_handler);
	csilk_app_get(app, "/secure-fail", auth_fail_handler);

	/* Mock AI Driver */
	csilk_ai_register_driver(&mock_ai_driver);
	g_mock_ai = csilk_ai_new("mock", nullptr, nullptr);
	if (g_mock_ai) {
		printf("[Init] Mock AI driver ready for dashboard AI stats\n");
	}

	/* Background MQ Timer */
	uv_timer_t mq_timer;
	uv_timer_init(csilk_io_default_loop(), &mq_timer);
	mq_timer.data = csilk_server_get_mq(csilk_app_server(app));
	uv_timer_start(&mq_timer, generate_bg_traffic, 1000, 5000);

	/* Background DB Timer */
	uv_timer_t db_timer;
	uv_timer_init(csilk_io_default_loop(), &db_timer);
	uv_timer_start(&db_timer, generate_db_traffic, 2000, 3000);

	/* Background AI Timer */
	uv_timer_t ai_timer;
	uv_timer_init(csilk_io_default_loop(), &ai_timer);
	uv_timer_start(&ai_timer, generate_ai_traffic, 3000, 4000);

	/* Background Security Timer */
	uv_timer_t sec_timer;
	uv_timer_init(csilk_io_default_loop(), &sec_timer);
	uv_timer_start(&sec_timer, generate_security_traffic, 1500, 6000);

	/* Background HTTP Traffic Timer */
	uv_timer_t http_timer;
	uv_timer_init(csilk_io_default_loop(), &http_timer);
	uv_timer_start(&http_timer, generate_http_traffic, 500, 2000);

	/* Multi-Core Configuration */
	csilk_server_config_t cfg = {0};
	cfg.worker_threads = 4;
	csilk_server_set_config(csilk_app_server(app), &cfg);

	csilk_app_run(app, 8080);

	/* Cleanup */
	if (g_mock_ai) {
		csilk_ai_free(g_mock_ai);
	}
	if (pool) {
		csilk_db_pool_free(pool);
	}
	csilk_app_free(app);
	curl_global_cleanup();

	return 0;
}
