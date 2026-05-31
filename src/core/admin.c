/**
 * @file admin.c
 * @brief Unified administration and monitoring controller implementation.
 */

#include "csilk/app/admin.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "csilk/core/internal.h"
#include "util/flamegraph.h"

/* Externs from metrics.c */
extern uint64_t atomic_load_http_requests(void);
extern uint64_t atomic_load_http_duration(void);

static uint64_t last_req_count = 0;
static uint64_t last_check_time = 0;

/** @brief Helper to serve the admin UI HTML. */
static void
admin_ui_handler(csilk_ctx_t* c)
{
	const char* paths[] = {"share/csilk/admin_ui.html",
			       "../share/csilk/admin_ui.html",
			       "/usr/local/share/csilk/admin_ui.html"};

	for (int i = 0; i < 3; i++) {
		FILE* f = fopen(paths[i], "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char* buf = malloc(sz + 1);
			if (buf) {
				if (fread(buf, 1, sz, f) == (size_t)sz) {
					buf[sz] = '\0';
					csilk_set_header(c, "Content-Type", "text/html");
					csilk_string(c, 200, buf);
				}
				free(buf);
			}
			fclose(f);
			return;
		}
	}
	csilk_string(c, 404, "Admin UI template not found.");
}

/** @brief Aggregates system-wide statistics into a JSON response. */
static void
admin_stats_handler(csilk_ctx_t* c)
{
	cJSON* root = cJSON_CreateObject();

	// 1. HTTP Metrics
	uint64_t total_req = csilk_metrics_get_total_requests();
	uint64_t total_dur = csilk_metrics_get_total_duration();
	double avg_latency = total_req > 0 ? (double)total_dur / total_req / 1000.0 : 0; // ms

	cJSON_AddNumberToObject(root, "total_requests", (double)total_req);
	cJSON_AddNumberToObject(root, "avg_latency", avg_latency);

	// 2. MQ Stats
	csilk_mq_stats_t mq_stats;
	csilk_mq_get_stats(csilk_server_get_mq(csilk_ctx_get_server(c)), &mq_stats);

	cJSON* mq = cJSON_CreateObject();
	cJSON_AddNumberToObject(mq, "published", (double)mq_stats.published_total);
	cJSON_AddNumberToObject(mq, "delivered", (double)mq_stats.delivered_total);
	cJSON_AddNumberToObject(mq, "depth", (double)mq_stats.queue_depth);
	cJSON_AddItemToObject(root, "mq", mq);

	// 3. AI Stats
	csilk_ai_stats_t ai_stats;
	csilk_ai_get_stats(&ai_stats);

	cJSON* ai = cJSON_CreateObject();
	cJSON_AddNumberToObject(ai, "requests", (double)ai_stats.requests_total);
	cJSON_AddNumberToObject(ai, "tokens", (double)ai_stats.tokens_total);
	cJSON_AddNumberToObject(ai, "errors", (double)ai_stats.errors_total);
	cJSON_AddNumberToObject(ai,
				"avg_duration",
				ai_stats.requests_total > 0 ? (double)ai_stats.duration_us_total /
								  ai_stats.requests_total / 1000.0
							    : 0);
	cJSON_AddItemToObject(root, "ai", ai);

	// 4. DB Stats
	csilk_db_stats_t db_stats;
	csilk_db_get_stats(&db_stats);

	cJSON* db = cJSON_CreateObject();
	cJSON_AddNumberToObject(db, "queries", (double)db_stats.queries_total);
	cJSON_AddNumberToObject(db, "execs", (double)db_stats.execs_total);
	cJSON_AddNumberToObject(db, "errors", (double)db_stats.errors_total);
	cJSON_AddNumberToObject(db,
				"avg_duration",
				db_stats.queries_total + db_stats.execs_total > 0
				    ? (double)db_stats.duration_us_total /
					  (db_stats.queries_total + db_stats.execs_total) / 1000.0
				    : 0);
	cJSON_AddItemToObject(root, "db", db);

	// 5. System Stats
	int active_conn = 0, pooled_conn = 0;
	csilk_server_get_stats(csilk_ctx_get_server(c), &active_conn, &pooled_conn);

	size_t arena_size = 0, arena_used = 0;
	csilk_arena_get_stats(csilk_get_arena(c), &arena_size, &arena_used);

	cJSON* sys = cJSON_CreateObject();
	cJSON_AddNumberToObject(sys, "active_connections", (double)active_conn);
	cJSON_AddNumberToObject(sys, "pooled_connections", (double)pooled_conn);
	cJSON_AddNumberToObject(sys, "arena_size_kb", (double)arena_size / 1024.0);
	cJSON_AddNumberToObject(sys, "arena_used_kb", (double)arena_used / 1024.0);
	cJSON_AddItemToObject(root, "sys", sys);

	// 6. Security Stats
	csilk_security_stats_t sec_stats;
	csilk_security_get_stats(&sec_stats);
	cJSON* sec = cJSON_CreateObject();
	cJSON_AddNumberToObject(sec, "rate_limit_blocks", (double)sec_stats.rate_limit_blocks);
	cJSON_AddNumberToObject(sec, "csrf_violations", (double)sec_stats.csrf_violations);
	cJSON_AddNumberToObject(sec, "auth_failures", (double)sec_stats.auth_failures);
	cJSON_AddItemToObject(root, "security", sec);

	// 7. Process Stats
	csilk_process_stats_t proc_stats;
	csilk_process_get_stats(&proc_stats);
	cJSON* proc = cJSON_CreateObject();
	cJSON_AddNumberToObject(proc, "rss_kb", (double)proc_stats.rss_bytes / 1024.0);
	cJSON_AddNumberToObject(proc, "cpu_user", proc_stats.cpu_user_time_sec);
	cJSON_AddNumberToObject(proc, "cpu_sys", proc_stats.cpu_sys_time_sec);
	cJSON_AddItemToObject(root, "process", proc);

	char* json = cJSON_PrintUnformatted(root);
	csilk_set_header(c, "Content-Type", "application/json");
	csilk_string(c, 200, json);

	free(json);
	cJSON_Delete(root);
}

/** @brief Returns the router topology for graph visualization. */
static void
admin_topology_handler(csilk_ctx_t* c)
{
	csilk_server_t* s = csilk_ctx_get_server(c);
	csilk_router_t* r = csilk_server_get_router(s);
	if (!r) {
		csilk_string(c, 404, "Router not found");
		return;
	}

	cJSON* routes = csilk_router_collect_routes(r);
	csilk_json(c, 200, routes);
}

/** @brief Start flame graph profiling and return the SVG result.
 *
 * Begins stack sampling for 5 seconds (100Hz), then stops and returns
 * the generated SVG flame graph directly as an image/svg+xml response.
 * Only one profiler session runs at a time. */
static void
admin_flamegraph_handler(csilk_ctx_t* c)
{
	if (csilk_flamegraph_is_running()) {
		csilk_json_error(c, 409, "Profiler already running — try again later");
		return;
	}

	if (csilk_flamegraph_start(10000) != 0) {
		csilk_json_error(c, 500, "Failed to start profiler");
		return;
	}

	usleep(5000000);

	char* svg = nullptr;
	size_t svg_len = 0;
	if (csilk_flamegraph_stop(&svg, &svg_len) == 0 && svg) {
		csilk_set_header(c, "Content-Type", "image/svg+xml");
		csilk_set_header(c, "Cache-Control", "no-cache");
		csilk_set_response_body(c, svg, svg_len, 1);
		csilk_string(c, 200, "");
		free(svg);
	} else {
		csilk_json_error(c, 500, "No samples collected");
	}
}

/** @brief Quick CPU profile — always available, non-blocking.
 *
 * Runs a brief 100ms sample and returns collapsed stacks as JSON.
 * Useful for dashboard widgets and automated monitoring. */
static void
admin_profile_quick_handler(csilk_ctx_t* c)
{
	if (csilk_flamegraph_is_running()) {
		cJSON* root = cJSON_CreateObject();
		cJSON_AddStringToObject(root, "status", "running");
		cJSON_AddStringToObject(root, "message", "Profiler is already running");
		csilk_json(c, 200, root);
		return;
	}

	if (csilk_flamegraph_start(5000) != 0) {
		csilk_json_error(c, 500, "Failed to start profiler");
		return;
	}
	usleep(100000);
	csilk_flamegraph_stop(nullptr, nullptr);
	csilk_json_error(c, 200, "Quick profile complete");
}

/** @brief WebSocket handler that streams multiplexed events. */
static void
admin_ws_handler(csilk_ctx_t* c)
{
	csilk_ws_handshake(c);
	if (csilk_is_websocket(c)) {
		csilk_mq_t* mq = csilk_server_get_mq(csilk_ctx_get_server(c));
		csilk_mq_register_monitor(mq, c);

		// Also monitor AI engine
		csilk_ai_register_monitor(c);

		printf("[Admin] Dashboard connected via WebSocket\n");
	}
}

void
csilk_admin_serve(csilk_app_t* app, const char* app_path)
{
	csilk_admin_serve_secure(app, app_path, nullptr);
}

/**
 * @brief Register administration routes with a secure middleware.
 *
 * This implementation creates a new route group for the admin panel, applying
 * an optional authentication middleware. This ensures that sensitive metrics
 * and real-time data are only accessible to authorized users.
 *
 * @param app             The high-level application handle.
 * @param app_path        The base URL path for the admin panel (e.g.,
 * "/admin").
 * @param auth_middleware Optional handler function for authentication.
 */
void
csilk_admin_serve_secure(csilk_app_t* app, const char* app_path, csilk_handler_t auth_middleware)
{
	if (!app || !app_path) {
		return;
	}

	/* Create a dedicated route group. This allows us to apply prefixing
     and middleware to all sub-routes (/stats, /ws) in one go. */
	csilk_group_t* group = csilk_group_new(csilk_app_router(app), app_path);
	if (!group) {
		return;
	}

	/* If a security middleware is provided (e.g., JWT validator), apply it. */
	if (auth_middleware) {
		csilk_group_use(group, auth_middleware);
	}

	/* Register sub-handlers. Because these are registered via the group,
     their paths are automatically relative to the group's prefix. */
	csilk_GET(group, "/", admin_ui_handler);		   /* GET /admin/ */
	csilk_GET(group, "/stats", admin_stats_handler);	   /* GET /admin/stats */
	csilk_GET(group, "/ws", admin_ws_handler);		   /* GET /admin/ws (Upgrade) */
	csilk_GET(group, "/topology", admin_topology_handler);	   /* GET /admin/topology */
	csilk_GET(group, "/flamegraph", admin_flamegraph_handler); /* GET /admin/flamegraph */
	csilk_GET(group, "/profile", admin_profile_quick_handler); /* GET /admin/profile */
}
