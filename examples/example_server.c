#include "csilk_internal.h"
/**
 * @file example_server.c
 * @brief Full-featured example server using the low-level core API.
 *
 * Demonstrates router, groups, middleware, WebSocket, SSE, multipart upload,
 * cookie handling, gzip compression, and YAML config loading.
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk.h"
#include "csilk_reflect.h"

// --- OpenAPI Demo: Reflected Structs ---

/** @brief Login request body type for reflection. */
typedef struct {
  char username[64];
  char password[64];
} login_request_t;

/** @brief Login response body type for reflection. */
typedef struct {
  char token[128];
  char message[256];
} login_response_t;

/** @brief User profile type for reflection. */
typedef struct {
  int id;
  char name[128];
  double score;
  bool active;
} user_profile_t;

// Field descriptor macros for the above structs
#define LOGIN_REQUEST_MAP(_, ...)                            \
  _(login_request_t, username, CSILK_TYPE_STRING,            \
    sizeof(((login_request_t*)0)->username), 0, false, NULL) \
  _(login_request_t, password, CSILK_TYPE_STRING,            \
    sizeof(((login_request_t*)0)->password), 0, false, NULL)

#define LOGIN_RESPONSE_MAP(_, ...)                         \
  _(login_response_t, token, CSILK_TYPE_STRING,            \
    sizeof(((login_response_t*)0)->token), 0, false, NULL) \
  _(login_response_t, message, CSILK_TYPE_STRING,          \
    sizeof(((login_response_t*)0)->message), 0, false, NULL)

#define USER_PROFILE_MAP(_, ...)                                              \
  _(user_profile_t, id, CSILK_TYPE_INT32, sizeof(int), 0, false, NULL)        \
  _(user_profile_t, name, CSILK_TYPE_STRING,                                  \
    sizeof(((user_profile_t*)0)->name), 0, false, NULL)                       \
  _(user_profile_t, score, CSILK_TYPE_DOUBLE, sizeof(double), 0, false, NULL) \
  _(user_profile_t, active, CSILK_TYPE_BOOL, sizeof(bool), 0, false, NULL)

// Auto-register structs with reflection at startup
CSILK_REGISTER_REFLECT(login_request_t, LOGIN_REQUEST_MAP)
CSILK_REGISTER_REFLECT(login_response_t, LOGIN_RESPONSE_MAP)
CSILK_REGISTER_REFLECT(user_profile_t, USER_PROFILE_MAP)

/** @brief Global router reference for OpenAPI handler. */
static csilk_router_t* g_router = NULL;

/** @brief OpenAPI spec handler - serves the generated spec at runtime. */
void openapi_spec_handler(csilk_ctx_t* c) {
  if (g_router) {
    csilk_serve_openapi(c, g_router, "csilk Demo API", "1.0.0",
                        "Example API demonstrating csilk framework features");
  } else {
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "Router not available");
  }
}

/** @brief Mock authentication validator — accepts only "secret123". */
int mock_auth_validator(const char* token) {
  return token && strcmp(token, "secret123") == 0;
}

/** @brief Handler for GET / — returns welcome message. */
void hello_handler(csilk_ctx_t* c) {
  csilk_string(c, CSILK_STATUS_OK,
               "Hello, World! Welcome to the C csilk Framework.");
}

/** @brief Handler for GET /user/:id — returns the user ID from the path param.
 */
void user_handler(csilk_ctx_t* c) {
  const char* user_id = csilk_get_param(c, "id");
  if (user_id) {
    char response[256];
    snprintf(response, sizeof(response), "User ID: %s", user_id);
    csilk_string(c, CSILK_STATUS_OK, response);
  } else {
    csilk_string(c, CSILK_STATUS_BAD_REQUEST, "Missing user ID");
  }
}

/** @brief Handler for POST /login — validates credentials and returns a token.
 */
void login_handler(csilk_ctx_t* c) {
  cJSON* json = csilk_bind_json(c);
  if (!json) {
    csilk_string(c, CSILK_STATUS_BAD_REQUEST, "Invalid JSON");
    return;
  }

  cJSON* username = cJSON_GetObjectItemCaseSensitive(json, "username");
  cJSON* password = cJSON_GetObjectItemCaseSensitive(json, "password");

  if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
    cJSON_Delete(json);
    csilk_string(c, CSILK_STATUS_BAD_REQUEST, "Username and password required");
    return;
  }

  if (strcmp(username->valuestring, "admin") == 0 &&
      strcmp(password->valuestring, "password") == 0) {
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "token", "secret123");
    cJSON_AddStringToObject(response, "message", "Login successful");
    csilk_json(c, CSILK_STATUS_OK, response);
  } else {
    csilk_string(c, CSILK_STATUS_UNAUTHORIZED, "Invalid credentials");
  }

  cJSON_Delete(json);
}

/** @brief Handler for GET /protected — returns content only if authorized. */
void protected_handler(csilk_ctx_t* c) {
  const char* token = csilk_get_header(c, "Authorization");
  if (!token || !mock_auth_validator(token)) {
    csilk_string(c, CSILK_STATUS_UNAUTHORIZED, "Unauthorized");
    return;
  }

  csilk_string(c, CSILK_STATUS_OK,
               "This is a protected resource. You have valid credentials.");
}

/** @brief Handler for GET /api/data — returns a JSON object with sample data.
 */
void api_data_handler(csilk_ctx_t* c) {
  cJSON* data = cJSON_CreateObject();
  cJSON_AddNumberToObject(data, "id", 1);
  cJSON_AddStringToObject(data, "name", "Sample Data");
  cJSON_AddBoolToObject(data, "active", true);

  cJSON* items = cJSON_CreateArray();
  cJSON_AddItemToArray(items, cJSON_CreateString("item1"));
  cJSON_AddItemToArray(items, cJSON_CreateString("item2"));
  cJSON_AddItemToArray(items, cJSON_CreateString("item3"));
  cJSON_AddItemToObject(data, "items", items);

  time_t now = time(NULL);
  cJSON_AddNumberToObject(data, "timestamp", (double)now);

  csilk_json(c, CSILK_STATUS_OK, data);
}

// --- New Feature Handlers ---

/** @brief SSE timer callback — sends counter events. */
static void sse_on_timer(csilk_ctx_t* c) {
  static int counter = 0;
  counter++;
  char data[128];
  snprintf(data, sizeof(data), "{\"counter\": %d, \"time\": %ld}", counter,
           (long)time(NULL));
  csilk_sse_send(c, "update", data);
  if (counter >= 5) {
    csilk_sse_close(c);
  }
}

/** @brief Handler for GET /events — SSE stream demo. */
void events_handler(csilk_ctx_t* c) {
  csilk_sse_init(c);

  // SSE data is sent via a periodic timer in a real app.
  // Here we send a few messages synchronously (for demo).
  csilk_sse_send(c, NULL, "Welcome to csilk SSE stream!");
  csilk_sse_send(c, "greeting", "{\"msg\": \"Hello from SSE\"}");
  csilk_sse_send(c, "done", NULL);
  csilk_sse_close(c);
}

/** @brief Multipart part handler — prints file/field info from uploads. */
void upload_part_handler(csilk_multipart_part_t* part) {
  if (part->filename[0] != '\0') {
    printf("[UPLOAD] File: %s, type: %s, size: %zu\n", part->filename,
           part->content_type, part->data_len);
  } else if (part->name[0] != '\0') {
    printf("[UPLOAD] Field: %s = %.*s\n", part->name, (int)part->data_len,
           part->data);
  }
}

/** @brief Handler for POST /upload — parses multipart form data. */
void upload_handler(csilk_ctx_t* c) {
  printf("[UPLOAD] Received multipart request\n");
  csilk_multipart_parse(c, upload_part_handler);
  csilk_string(c, CSILK_STATUS_OK, "Upload received");
}

/** @brief Handler for GET /large — returns a sizable response for gzip demo. */
void large_handler(csilk_ctx_t* c) {
  // Generate a ~4KB response (good for gzip demo)
  static char buf[4096];
  memset(buf, 'X', sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  for (int i = 0; i < 1024; i += 64) {
    memcpy(buf + i, "This is a repetitive line that compresses very well. ",
           53);
  }
  csilk_string(c, CSILK_STATUS_OK, buf);
}

/** @brief Handler for GET/POST /cookie — set, read, or delete a demo cookie. */
void cookie_handler(csilk_ctx_t* c) {
  const char* action = csilk_get_query(c, "action");
  if (action && strcmp(action, "set") == 0) {
    csilk_set_cookie(c, "demo_session", "abc123", 3600, "/", NULL, 1, 1);
    csilk_string(c, CSILK_STATUS_OK, "Cookie set!");
  } else if (action && strcmp(action, "delete") == 0) {
    csilk_set_cookie(c, "demo_session", "deleted", -1, "/", NULL, 0, 0);
    csilk_string(c, CSILK_STATUS_OK, "Cookie deleted!");
  } else {
    const char* val = csilk_get_cookie(c, "demo_session");
    char buf[256];
    snprintf(buf, sizeof(buf), "demo_session = %s", val ? val : "not set");
    csilk_string(c, CSILK_STATUS_OK, buf);
  }
}

/** @brief Custom middleware — logs a sequential request ID. */
void request_id_middleware(csilk_ctx_t* c) {
  static int request_counter = 0;
  request_counter++;
  printf("[MIDDLEWARE] Request ID: %d\n", request_counter);
  csilk_next(c);
}

/** @brief Custom middleware — measures and logs request processing time. */
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
    cfg.port = 8080;
    cfg.logger.level = CSILK_LOG_DEBUG;
    cfg.logger.file_path = NULL;
    cfg.logger.max_file_size = 0;
    cfg.logger.use_colors = -1;
    cfg.server.idle_timeout_ms = 5000;
    cfg.server.max_body_size = 1024 * 1024;
    cfg.server.listen_backlog = 128;
    cfg.static_files.enable = 0;
    cfg.rate_limit.enable = 0;
  }

  // Validate config
  const char* err_msg = NULL;
  if (csilk_config_validate(&cfg, &err_msg) != 0) {
    printf("Config validation error: %s\n", err_msg);
    return 1;
  }

  // Initialize logger from config
  csilk_log_init(cfg.logger);
  CSILK_LOG_I("Starting csilk Framework Example Server...");

  // Create router
  csilk_router_t* router = csilk_router_new();
  if (!router) {
    CSILK_LOG_E("Failed to create router");
    return 1;
  }
  g_router = router;  // For OpenAPI handler

  // Create server first to use csilk_server_use
  csilk_server_t* server = csilk_server_new(router);
  if (!server) {
    CSILK_LOG_E("Failed to create server");
    csilk_router_free(router);
    return 1;
  }

  // Register global middlewares
  csilk_server_use(server, csilk_recovery_handler);
  csilk_server_use(server, csilk_logger_handler);

  // Root route group
  csilk_group_t* root = csilk_group_new(router, "");

  // API group with gzip compression
  csilk_group_t* api_group = csilk_group_group(root, "/api");
  if (!api_group) {
    CSILK_LOG_E("Failed to create API group");
    csilk_router_free(router);
    return 1;
  }
  csilk_group_use(api_group, csilk_gzip_middleware);
  csilk_GET(api_group, "/data", api_data_handler);
  csilk_GET(api_group, "/large", large_handler);

  // Protected group
  csilk_group_t* protected_group = csilk_group_group(root, "");
  if (!protected_group) {
    CSILK_LOG_E("Failed to create protected group");
    csilk_group_free(api_group);
    csilk_router_free(router);
    return 1;
  }
  csilk_GET(protected_group, "/protected", protected_handler);

  // Root routes with OpenAPI metadata
  csilk_GET(root, "/", hello_handler);
  csilk_GET(root, "/user/:id", user_handler);

  // Register POST /login with input/output types for OpenAPI
  csilk_handler_t login_handlers[] = {login_handler};
  csilk_router_add_extended(
      router, "POST", "/login", login_handlers, 1, "/login",
      "login_request_t",   // input_type
      "login_response_t",  // output_type
      "User Login",        // summary
      "Authenticate with username/password and receive a token");

  csilk_GET(root, "/events", events_handler);
  csilk_POST(root, "/upload", upload_handler);
  csilk_GET(root, "/cookie", cookie_handler);
  csilk_POST(root, "/cookie", cookie_handler);

  // OpenAPI spec endpoint (use raw router_add to avoid registration)
  csilk_handler_t openapi_handlers[] = {openapi_spec_handler};
  csilk_router_add_extended(
      router, "GET", "/openapi.json", openapi_handlers, 1, "/openapi.json",
      NULL, NULL, "OpenAPI Specification",
      "Returns the OpenAPI 3.0 JSON specification for this API");

  // Static file serving (if configured)
  if (cfg.static_files.enable && cfg.static_files.root_dir) {
    const char* prefix =
        cfg.static_files.prefix ? cfg.static_files.prefix : "/static";
    char route[256];
    snprintf(route, sizeof(route), "%s/*path", prefix);
    CSILK_LOG_I("Static files: %s -> %s", route, cfg.static_files.root_dir);
    csilk_handler_t handlers[] = {csilk_logger_handler, NULL};
    csilk_group_add_handlers(root, "GET", route, handlers, 1);
  }

  // Rate limiting
  if (cfg.rate_limit.enable) {
    CSILK_LOG_I("Rate limiting: %d req/min",
                cfg.rate_limit.requests_per_minute);
  }

  csilk_server_set_config(server, cfg.server);

  printf("\n========================================\n");
  printf("csilk Server running on port %d\n", cfg.port);
  printf("========================================\n");
  printf("Endpoints:\n");
  printf("  GET  /                   Hello World\n");
  printf("  GET  /user/:id            Path parameter demo\n");
  printf("  POST /login               JSON login (admin/password)\n");
  printf(
      "  GET  /protected           Auth-protected (Authorization: "
      "secret123)\n");
  printf("  GET  /api/data            JSON API response\n");
  printf("  GET  /api/large           Large response (gzip demo)\n");
  printf("  GET  /events              Server-Sent Events demo\n");
  printf("  POST /upload              Multipart file upload\n");
  printf("  GET  /cookie?action=set   Set demo session cookie\n");
  printf("  GET  /cookie?action=delete Delete demo session cookie\n");
  printf("  GET  /cookie              Read demo session cookie\n");
  if (cfg.static_files.enable) {
    printf("  GET  %s/*path            Static files from %s\n",
           cfg.static_files.prefix ? cfg.static_files.prefix : "/static",
           cfg.static_files.root_dir);
  }
  printf("========================================\n");
  printf("Press Ctrl+C to stop\n\n");

  int result = csilk_server_run(server, cfg.port);

  // Cleanup
  csilk_log_close();
  csilk_server_free(server);
  csilk_group_free(api_group);
  csilk_group_free(protected_group);
  csilk_group_free(root);
  csilk_router_free(router);
  csilk_config_free(&cfg);

  return result;
}