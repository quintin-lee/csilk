/**
 * @file admin.c
 * @brief Unified administration and monitoring controller implementation.
 */

#include "csilk/app/admin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#include "cJSON.h"
#include "csilk/core/internal.h"

/* Externs from metrics.c */
extern uint64_t atomic_load_http_requests(void);
extern uint64_t atomic_load_http_duration(void);

static uint64_t last_req_count = 0;
static uint64_t last_check_time = 0;

/** @brief Helper to serve the admin UI HTML. */
static void admin_ui_handler(csilk_ctx_t* c) {
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
static void admin_stats_handler(csilk_ctx_t* c) {
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
    
    char* json = cJSON_PrintUnformatted(root);
    csilk_set_header(c, "Content-Type", "application/json");
    csilk_string(c, 200, json);
    
    free(json);
    cJSON_Delete(root);
}

/** @brief WebSocket handler that streams multiplexed events. */
static void admin_ws_handler(csilk_ctx_t* c) {
    csilk_ws_handshake(c);
    if (csilk_is_websocket(c)) {
        csilk_mq_t* mq = csilk_server_get_mq(csilk_ctx_get_server(c));
        csilk_mq_register_monitor(mq, c);
        
        // In a real implementation, we'd also register this ctx 
        // to a global "admin_monitors" list to receive ALL workflow events.
        printf("[Admin] Dashboard connected via WebSocket\n");
    }
}

void csilk_admin_serve(csilk_app_t* app, const char* path) {
    if (!app || !path) return;
    
    char stats_path[256];
    char ws_path[256];
    snprintf(stats_path, sizeof(stats_path), "%s/stats", path);
    snprintf(ws_path, sizeof(ws_path), "%s/ws", path);
    
    csilk_app_get(app, path, admin_ui_handler);
    csilk_app_get(app, stats_path, admin_stats_handler);
    csilk_app_get(app, ws_path, admin_ws_handler);
}
