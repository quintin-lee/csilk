#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "gin.h"

// Mock authentication validator
int mock_auth_validator(const char* token) {
    // Simple token validation for demo
    return token && strcmp(token, "secret123") == 0;
}

// Handler for GET /
void hello_handler(gin_ctx_t* c) {
    gin_string(c, 200, "Hello, World! Welcome to the C Gin Framework.");
}

// Handler for GET /user/:id
void user_handler(gin_ctx_t* c) {
    const char* user_id = gin_get_param(c, "id");
    if (user_id) {
        char response[256];
        snprintf(response, sizeof(response), "User ID: %s", user_id);
        gin_string(c, 200, response);
    } else {
        gin_string(c, 400, "Missing user ID");
    }
}

// Handler for POST /login
void login_handler(gin_ctx_t* c) {
    // Parse JSON body
    cJSON* json = gin_bind_json(c);
    if (!json) {
        gin_string(c, 400, "Invalid JSON");
        return;
    }
    
    // Extract username and password
    cJSON* username = cJSON_GetObjectItemCaseSensitive(json, "username");
    cJSON* password = cJSON_GetObjectItemCaseSensitive(json, "password");
    
    if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
        cJSON_Delete(json);
        gin_string(c, 400, "Username and password required");
        return;
    }
    
    // Simple credential check (for demo only)
    if (strcmp(username->valuestring, "admin") == 0 && 
        strcmp(password->valuestring, "password") == 0) {
        
        // Return success with token
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "token", "secret123");
        cJSON_AddStringToObject(response, "message", "Login successful");
        gin_json(c, 200, response);
        cJSON_Delete(response);
    } else {
        gin_string(c, 401, "Invalid credentials");
    }
    
    cJSON_Delete(json);
}

// Handler for GET /protected (requires auth)
void protected_handler(gin_ctx_t* c) {
    const char* token = gin_get_header(c, "Authorization");
    if (!token || !mock_auth_validator(token)) {
        gin_string(c, 401, "Unauthorized");
        return;
    }
    
    gin_string(c, 200, "This is a protected resource. You have valid credentials.");
}

// Handler for GET /api/data
void api_data_handler(gin_ctx_t* c) {
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
    
    gin_json(c, 200, data);
    cJSON_Delete(data);
}

// Custom middleware to add request ID
void request_id_middleware(gin_ctx_t* c) {
    // In a real implementation, you'd generate a proper UUID
    static int request_counter = 0;
    request_counter++;
    
    // For demo, we'll just log it
    printf("[MIDDLEWARE] Request ID: %d\n", request_counter);
    
    gin_next(c);
}

// Custom middleware to log request time
void request_timer_middleware(gin_ctx_t* c) {
    clock_t start = clock();
    gin_next(c);
    clock_t end = clock();
    
    double duration = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("[TIMER] Request processed in %.4f seconds\n", duration);
}

int main() {
    printf("Starting C Gin Framework Example Server...\n");
    
    // Create router
    gin_router_t* router = gin_router_new();
    if (!router) {
        fprintf(stderr, "Failed to create router\n");
        return 1;
    }
    
    // Create API group
    gin_group_t* api_group = gin_group_new(router, "/api");
    if (!api_group) {
        fprintf(stderr, "Failed to create API group\n");
        gin_router_free(router);
        return 1;
    }
    
    // Add routes to API group
    gin_GET(api_group, "/data", api_data_handler);
    
    // Create protected group with auth middleware
    gin_group_t* protected_group = gin_group_new(router, "");
    if (!protected_group) {
        fprintf(stderr, "Failed to create protected group\n");
        gin_group_free(api_group);
        gin_router_free(router);
        return 1;
    }
    
    // Apply auth middleware to protected group
    gin_group_use(protected_group, (gin_handler_t)gin_auth_middleware);
    // Note: In a real implementation, we'd need to properly curry the validator function
    // For this demo, we'll handle auth directly in the handler
    
    gin_GET(protected_group, "/protected", protected_handler);
    
    // Add routes to main router
    gin_handler_t hello_handlers[] = {hello_handler};
    gin_router_add(router, "GET", "/", hello_handlers, 1);
    
    gin_handler_t user_handlers[] = {user_handler};
    gin_router_add(router, "GET", "/user/:id", user_handlers, 1);
    
    gin_handler_t login_handlers[] = {login_handler};
    gin_router_add(router, "POST", "/login", login_handlers, 1);
    
    // Create server
    gin_server_t* server = gin_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        gin_group_free(api_group);
        gin_group_free(protected_group);
        gin_router_free(router);
        return 1;
    }
    
    // Run server on port 8080
    printf("Server running on http://localhost:8080\n");
    printf("Try these endpoints:\n");
    printf("  GET  /\n");
    printf("  GET  /user/123\n");
    printf("  POST /login (with JSON: {\"username\":\"admin\",\"password\":\"password\"})\n");
    printf("  GET  /protected (with header: Authorization: secret123)\n");
    printf("  GET  /api/data\n");
    printf("Press Ctrl+C to stop\n");
    
    int result = gin_server_run(server, 8080);
    
    // Cleanup
    gin_server_free(server);
    gin_group_free(api_group);
    gin_group_free(protected_group);
    gin_router_free(router);
    
    return result;
}