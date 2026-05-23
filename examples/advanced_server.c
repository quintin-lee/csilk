#include "csilk_internal.h"
/**
 * @file advanced_server.c
 * @brief Advanced server example with route groups, WebSocket, and auth middleware.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "csilk.h"

/** @brief Example user model struct. */
typedef struct {
    int id;           /**< User ID number. */
    char name[100];   /**< User display name. */
} user_t;

/** @brief Custom auth middleware — validates Authorization header. */
void auth_required(csilk_ctx_t* c) {
    const char* token = csilk_get_header(c, "Authorization");
    if (token && strcmp(token, "secret-token") == 0) {
        csilk_next(c);
    } else {
        csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Unauthorized: Invalid or missing token");
        csilk_abort(c);
    }
}

/** @brief Handler for GET /api/v1/users/:id — returns a mock user. */
void get_user_handler(csilk_ctx_t* c) {
    const char* id = csilk_get_param(c, "id");
    
    cJSON* user = cJSON_CreateObject();
    cJSON_AddNumberToObject(user, "id", atoi(id));
    cJSON_AddStringToObject(user, "name", "John Doe");
    cJSON_AddStringToObject(user, "email", "john@example.com");
    
    csilk_json(c, CSILK_STATUS_OK, user);
}

/** @brief Handler for POST /api/v1/data — echoes received JSON with a 50ms delay. */
void post_data_handler(csilk_ctx_t* c) {
    cJSON* json = csilk_bind_json(c);
    if (!json) {
        csilk_json_error(c, CSILK_STATUS_BAD_REQUEST, "Bad Request: Invalid JSON");
        return;
    }
    
    /* simulate processing delay */
    struct timespec ts = {0, 50 * 1000000}; // 50ms
    nanosleep(&ts, NULL);
    
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "received");
    cJSON_AddItemToObject(resp, "data", cJSON_Duplicate(json, 1));
    
    csilk_json(c, CSILK_STATUS_CREATED, resp);
    cJSON_Delete(json);
}

/** @brief WebSocket message callback — echoes received messages back to client. */
void ws_on_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    printf("WebSocket Message: %s\n", (char*)payload);
    char reply[256];
    snprintf(reply, sizeof(reply), "Server received: %s", (char*)payload);
    csilk_ws_send(c, (uint8_t*)reply, strlen(reply), opcode);
}

/** @brief WebSocket upgrade handler — performs handshake and sets message callback. */
void ws_handler(csilk_ctx_t* c) {
    csilk_ws_handshake(c);
    if (c->is_websocket) {
        c->on_ws_message = ws_on_message;
    }
}

/** @brief Simple ping handler — returns "pong". */
void ping_handler(csilk_ctx_t* c) {
    csilk_string(c, CSILK_STATUS_OK, "pong");
}

/** @brief Static file handler — serves files from the ./public directory. */
void static_handler(csilk_ctx_t* c) {
    csilk_static(c, "./public");
}

int main() {
    // Initialize logger
    csilk_log_config_t log_cfg = {
        .level = CSILK_LOG_DEBUG,
        .file_path = NULL,
        .max_file_size = 0,
        .use_colors = -1
    };
    csilk_log_init(log_cfg);

    /* create router */
    csilk_router_t* router = csilk_router_new();
    
    /* root group with recovery and logger middleware */
    csilk_group_t* root = csilk_group_new(router, "");
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);
    
    csilk_GET(root, "/ping", ping_handler);

    /* WebSocket route */
    csilk_GET(root, "/ws", ws_handler);

    /* API group with custom auth middleware */
    csilk_group_t* api = csilk_group_group(root, "/api/v1");
    csilk_group_use(api, auth_required);
    
    csilk_GET(api, "/users/:id", get_user_handler);
    csilk_POST(api, "/data", post_data_handler);

    /* static file serving */
    csilk_GET(root, "/static/*path", static_handler);

    /* start server */
    printf("Server-C Advanced Example starting on port 8080...\n");
    printf("Test with: curl -H 'Authorization: secret-token' http://localhost:8080/api/v1/users/42\n");
    
    csilk_server_t* server = csilk_server_new(router);
    csilk_server_run(server, 8080);
    
    /* cleanup */
    csilk_server_free(server);
    csilk_group_free(api);
    csilk_group_free(root);
    csilk_router_free(router);
    csilk_config_free(&cfg);
    
    return 0;
}
