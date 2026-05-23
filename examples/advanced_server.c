#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gin.h"

// 1. 定义业务模型
typedef struct {
    int id;
    char name[100];
} user_t;

// 2. 自定义中间件：认证
void auth_required(gin_ctx_t* c) {
    const char* token = gin_get_header(c, "Authorization");
    if (token && strcmp(token, "secret-token") == 0) {
        gin_next(c);
    } else {
        gin_json_error(c, 401, "Unauthorized: Invalid or missing token");
        gin_abort(c);
    }
}

// 3. 业务处理器
void get_user_handler(gin_ctx_t* c) {
    const char* id = gin_get_param(c, "id");
    
    cJSON* user = cJSON_CreateObject();
    cJSON_AddNumberToObject(user, "id", atoi(id));
    cJSON_AddStringToObject(user, "name", "John Doe");
    cJSON_AddStringToObject(user, "email", "john@example.com");
    
    gin_json(c, 200, user);
}

void post_data_handler(gin_ctx_t* c) {
    cJSON* json = gin_bind_json(c);
    if (!json) {
        gin_json_error(c, 400, "Bad Request: Invalid JSON");
        return;
    }
    
    // 模拟处理耗时
    struct timespec ts = {0, 50 * 1000000}; // 50ms
    nanosleep(&ts, NULL);
    
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "received");
    cJSON_AddItemToObject(resp, "data", cJSON_Duplicate(json, 1));
    
    gin_json(c, 201, resp);
    cJSON_Delete(json);
}

// 4. WebSocket 处理器
void ws_on_message(gin_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    printf("WebSocket Message: %s\n", (char*)payload);
    char reply[256];
    snprintf(reply, sizeof(reply), "Server received: %s", (char*)payload);
    gin_ws_send(c, (uint8_t*)reply, strlen(reply), opcode);
}

void ws_handler(gin_ctx_t* c) {
    gin_ws_handshake(c);
    if (c->is_websocket) {
        c->on_ws_message = ws_on_message;
    }
}

int main() {
    // Initialize logger
    gin_log_init(GIN_LOG_DEBUG, NULL);

    // 初始化路由
    gin_router_t* router = gin_router_new();
    
    // 根组：使用 Recovery 和 Logger 基础中间件
    gin_group_t* root = gin_group_new(router, "");
    gin_group_use(root, gin_recovery_handler);
    gin_group_use(root, gin_logger_handler);
    
    gin_GET(root, "/ping", (gin_handler_t)[](gin_ctx_t* c){
        gin_string(c, 200, "pong");
    });

    // WebSocket 路由
    gin_GET(root, "/ws", ws_handler);

    // API 组：使用自定义 Auth 中间件
    gin_group_t* api = gin_group_group(root, "/api/v1");
    gin_group_use(api, auth_required);
    
    gin_GET(api, "/users/:id", get_user_handler);
    gin_POST(api, "/data", post_data_handler);

    // 静态文件服务
    gin_GET(root, "/static/*path", (gin_handler_t)[](gin_ctx_t* c){
        gin_static(c, "./public");
    });

    // 启动服务器
    printf("Server-C Advanced Example starting on port 8080...\n");
    printf("Test with: curl -H 'Authorization: secret-token' http://localhost:8080/api/v1/users/42\n");
    
    gin_server_t* server = gin_server_new(router);
    gin_server_run(server, 8080);
    
    // 资源清理
    gin_server_free(server);
    gin_router_free(router);
    
    return 0;
}
