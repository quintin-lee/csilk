#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "csilk.h"

// 1. 定义业务模型
typedef struct {
    int id;
    char name[100];
} user_t;

// 2. 自定义中间件：认证
void auth_required(csilk_ctx_t* c) {
    const char* token = csilk_get_header(c, "Authorization");
    if (token && strcmp(token, "secret-token") == 0) {
        csilk_next(c);
    } else {
        csilk_json_error(c, 401, "Unauthorized: Invalid or missing token");
        csilk_abort(c);
    }
}

// 3. 业务处理器
void get_user_handler(csilk_ctx_t* c) {
    const char* id = csilk_get_param(c, "id");
    
    cJSON* user = cJSON_CreateObject();
    cJSON_AddNumberToObject(user, "id", atoi(id));
    cJSON_AddStringToObject(user, "name", "John Doe");
    cJSON_AddStringToObject(user, "email", "john@example.com");
    
    csilk_json(c, 200, user);
}

void post_data_handler(csilk_ctx_t* c) {
    cJSON* json = csilk_bind_json(c);
    if (!json) {
        csilk_json_error(c, 400, "Bad Request: Invalid JSON");
        return;
    }
    
    // 模拟处理耗时
    struct timespec ts = {0, 50 * 1000000}; // 50ms
    nanosleep(&ts, NULL);
    
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "received");
    cJSON_AddItemToObject(resp, "data", cJSON_Duplicate(json, 1));
    
    csilk_json(c, 201, resp);
    cJSON_Delete(json);
}

// 4. WebSocket 处理器
void ws_on_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    printf("WebSocket Message: %s\n", (char*)payload);
    char reply[256];
    snprintf(reply, sizeof(reply), "Server received: %s", (char*)payload);
    csilk_ws_send(c, (uint8_t*)reply, strlen(reply), opcode);
}

void ws_handler(csilk_ctx_t* c) {
    csilk_ws_handshake(c);
    if (c->is_websocket) {
        c->on_ws_message = ws_on_message;
    }
}

int main() {
    // Initialize logger
    csilk_log_init(CSILK_LOG_DEBUG, NULL);

    // 初始化路由
    csilk_router_t* router = csilk_router_new();
    
    // 根组：使用 Recovery 和 Logger 基础中间件
    csilk_group_t* root = csilk_group_new(router, "");
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);
    
    csilk_GET(root, "/ping", (csilk_handler_t)[](csilk_ctx_t* c){
        csilk_string(c, 200, "pong");
    });

    // WebSocket 路由
    csilk_GET(root, "/ws", ws_handler);

    // API 组：使用自定义 Auth 中间件
    csilk_group_t* api = csilk_group_group(root, "/api/v1");
    csilk_group_use(api, auth_required);
    
    csilk_GET(api, "/users/:id", get_user_handler);
    csilk_POST(api, "/data", post_data_handler);

    // 静态文件服务
    csilk_GET(root, "/static/*path", (csilk_handler_t)[](csilk_ctx_t* c){
        csilk_static(c, "./public");
    });

    // 启动服务器
    printf("Server-C Advanced Example starting on port 8080...\n");
    printf("Test with: curl -H 'Authorization: secret-token' http://localhost:8080/api/v1/users/42\n");
    
    csilk_server_t* server = csilk_server_new(router);
    csilk_server_run(server, 8080);
    
    // 资源清理
    csilk_server_free(server);
    csilk_router_free(router);
    
    return 0;
}
