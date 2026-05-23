#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "csilk.h"

// Mock authentication validator
int mock_auth_validator(const char* token) {
    // Simple token validation for demo
    return token && strcmp(token, "secret123") == 0;
}

// Handler for GET /
void hello_handler(csilk_ctx_t* c) {
    csilk_string(c, 200, "Hello, World! Welcome to the C Csilk Framework.");
}

// Handler for GET /user/:id
void user_handler(csilk_ctx_t* c) {
    const char* user_id = csilk_get_param(c, "id");
    if (user_id) {
        char response[256];
        snprintf(response, sizeof(response), "User ID: %s", user_id);
        csilk_string(c, 200, response);
    } else {
        csilk_string(c, 400, "Missing user ID");
    }
}

// Handler for POST /login
void login_handler(csilk_ctx_t* c) {
    // Parse JSON body
    cJSON* json = csilk_bind_json(c);
    if (!json) {
        csilk_string(c, 400, "Invalid JSON");
        return;
    }
    
    // Extract username and password
    cJSON* username = cJSON_GetObjectItemCaseSensitive(json, "username");
    cJSON* password = cJSON_GetObjectItemCaseSensitive(json, "password");
    
    if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
        cJSON_Delete(json);
        csilk_string(c, 400, "Username and password required");
        return;
    }
    
    // Simple credential check (for demo only)
    if (strcmp(username->valuestring, "admin") == 0 && 
        strcmp(password->valuestring, "password") == 0) {
        
        // Return success with token
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "token", "secret123");
        cJSON_AddStringToObject(response, "message", "Login successful");
        csilk_json(c, 200, response);
    } else {
        csilk_string(c, 401, "Invalid credentials");
    }
    
    cJSON_Delete(json);
}

// Handler for GET /protected (requires auth)
void protected_handler(csilk_ctx_t* c) {
    const char* token = csilk_get_header(c, "Authorization");
    if (!token || !mock_auth_validator(token)) {
        csilk_string(c, 401, "Unauthorized");
        return;
    }
    
    csilk_string(c, 200, "This is a protected resource. You have valid credentials.");
}

// Handler for GET /api/data
void api_data_handler(csilk_ctx_t* c) {
    // Create sample data
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", 1);
    cJSON_AddStringToObject(data, "name", "Sample Data");
    cJSON_AddBoolToObject(data, "active", true);
    
    cJSON* items = cJSON_CreateArray();
    cJSON_AddItemToArray(items, cJSON_CreateString("item1"));
    cJSON_AddItemToArray(items, cJSON_CreateString("item2"));
    cJSON_AddItemToArray(items, cJSON_CreateString("item3"));
    cJSON_AddItemToObject(data, "items", items);
    
    // Add timestamp
    time_t now = time(NULL);
    cJSON_AddNumberToObject(data, "timestamp", (double)now);
    
    csilk_json(c, 200, data);
}

// Custom middleware to add request ID
void request_id_middleware(csilk_ctx_t* c) {
    // In a real implementation, you'd generate a proper UUID
    static int request_counter = 0;
    request_counter++;
    
    // For demo, we'll just log it
    printf("[MIDDLEWARE] Request ID: %d\n", request_counter);
    
    csilk_next(c);
}

// Custom middleware to log request time
void request_timer_middleware(csilk_ctx_t* c) {
    clock_t start = clock();
    csilk_next(c);
    clock_t end = clock();
    
    double duration = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("[TIMER] Request processed in %.4f seconds\n", duration);
}

int main(int argc, char* argv[]) {
    csilk_config_t cfg;
    const char* config_file = "config.yaml";
    
    if (argc > 1) {
        config_file = argv[1];
    }

    printf("Loading config from %s...\n", config_file);
    if (csilk_load_config(config_file, &cfg) != 0) {
        printf("Config file not found or invalid, using defaults.\n");
        // Manual defaults if config fails
        cfg.port = 8080;
        cfg.logger.level = CSILK_LOG_DEBUG;
        cfg.logger.file_path = NULL;
        cfg.logger.max_file_size = 0;
        cfg.logger.use_colors = -1;
        cfg.server.idle_timeout_ms = 5000;
        cfg.server.max_body_size = 1024 * 1024;
        cfg.server.listen_backlog = 128;
    }

    // Initialize logger from config
    csilk_log_init(cfg.logger);
    CSILK_LOG_I("Starting C Csilk Framework Example Server...");
    
    // Create router
    csilk_router_t* router = csilk_router_new();
    if (!router) {
        CSILK_LOG_E("Failed to create router");
        return 1;
    }
    
    // Global middleware group
    csilk_group_t* root = csilk_group_new(router, "");
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);

    if (cfg.rate_limit.enable) {
        CSILK_LOG_I("Rate limiting enabled: %d req/min", cfg.rate_limit.requests_per_minute);
        // Note: For real use, we'd need a way to pass the limit to the middleware properly
        // For now we just show it's enabled in the log
    }

    // Create API group
    csilk_group_t* api_group = csilk_group_group(root, "/api");
    if (!api_group) {
        CSILK_LOG_E("Failed to create API group");
        csilk_router_free(router);
        return 1;
    }
    
    // Add routes to API group
    csilk_GET(api_group, "/data", api_data_handler);
    
    // Create protected group (auth will be handled in the handler)
    csilk_group_t* protected_group = csilk_group_group(root, "");
    if (!protected_group) {
        CSILK_LOG_E("Failed to create protected group");
        csilk_group_free(api_group);
        csilk_router_free(router);
        return 1;
    }
    
    csilk_GET(protected_group, "/protected", protected_handler);
    
    // Add routes to root
    csilk_GET(root, "/", hello_handler);
    csilk_GET(root, "/user/:id", user_handler);
    csilk_POST(root, "/login", login_handler);
    
    if (cfg.static_files.enable && cfg.static_files.root_dir) {
        const char* prefix = cfg.static_files.prefix ? cfg.static_files.prefix : "/static";
        char route[256];
        snprintf(route, sizeof(route), "%s/*path", prefix);
        CSILK_LOG_I("Static files enabled: %s -> %s", route, cfg.static_files.root_dir);
        // csilk_GET(root, route, ...); // Needs a way to bind root_dir
    }

    // Create server
    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        CSILK_LOG_E("Failed to create server");
        csilk_group_free(api_group);
        csilk_group_free(protected_group);
        csilk_group_free(root);
        csilk_router_free(router);
        return 1;
    }

    // Apply server config
    csilk_server_set_config(server, cfg.server);
    
    printf("Server running on http://localhost:%d\n", cfg.port);
    printf("Try these endpoints:\n");
    printf("  GET  /\n");
    printf("  GET  /user/123\n");
    printf("  POST /login (with JSON: {\"username\":\"admin\",\"password\":\"password\"})\n");
    printf("  GET  /protected (with header: Authorization: secret123)\n");
    printf("  GET  /api/data\n");
    printf("Press Ctrl+C to stop\n");
    
    int result = csilk_server_run(server, cfg.port);
    
    // Cleanup
    csilk_log_close();
    csilk_server_free(server);
    csilk_group_free(api_group);
    csilk_group_free(protected_group);
    csilk_group_free(root);
    csilk_router_free(router);
    
    return result;
}