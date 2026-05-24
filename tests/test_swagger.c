#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "context_internal.h"
#include "csilk_internal.h"
#include "csilk_reflect.h"

// Test structs for reflection-based schema generation
typedef struct {
  int32_t id;
  char name[64];
  double price;
  bool active;
} Product;

typedef struct {
  char username[64];
  char password[64];
} LoginReq;

typedef struct {
  char token[128];
  char message[256];
} LoginResp;

#define PRODUCT_MAP(X)                                                       \
  X(Product, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, NULL)          \
  X(Product, name, CSILK_TYPE_STRING, sizeof(((Product*)0)->name), 0, false, \
    NULL)                                                                    \
  X(Product, price, CSILK_TYPE_DOUBLE, sizeof(double), 0, false, NULL)       \
  X(Product, active, CSILK_TYPE_BOOL, sizeof(bool), 0, false, NULL)

#define LOGINREQ_MAP(X)                                                      \
  X(LoginReq, username, CSILK_TYPE_STRING, sizeof(((LoginReq*)0)->username), \
    0, false, NULL)                                                          \
  X(LoginReq, password, CSILK_TYPE_STRING, sizeof(((LoginReq*)0)->password), \
    0, false, NULL)

#define LOGINRESP_MAP(X)                                                     \
  X(LoginResp, token, CSILK_TYPE_STRING, sizeof(((LoginResp*)0)->token), 0,  \
    false, NULL)                                                             \
  X(LoginResp, message, CSILK_TYPE_STRING, sizeof(((LoginResp*)0)->message), \
    0, false, NULL)

CSILK_REGISTER_REFLECT(Product, PRODUCT_MAP)
CSILK_REGISTER_REFLECT(LoginReq, LOGINREQ_MAP)
CSILK_REGISTER_REFLECT(LoginResp, LOGINRESP_MAP)

// Dummy handlers
void dummy_handler(csilk_ctx_t* c) { csilk_string(c, 200, "ok"); }

// --- Helper to verify cJSON tree ---

static int json_has_string(cJSON* obj, const char* key, const char* val) {
  cJSON* item = cJSON_GetObjectItem(obj, key);
  return item && cJSON_IsString(item) && strcmp(item->valuestring, val) == 0;
}

static cJSON* get_path(cJSON* spec, const char* method, const char* path) {
  cJSON* paths = cJSON_GetObjectItem(spec, "paths");
  if (!paths) return NULL;
  cJSON* pobj = cJSON_GetObjectItem(paths, path);
  if (!pobj) return NULL;
  return cJSON_GetObjectItem(pobj, method);
}

static int count_array(cJSON* arr) {
  if (!arr || !cJSON_IsArray(arr)) return 0;
  int c = 0;
  cJSON* e;
  cJSON_ArrayForEach(e, arr) {
    (void)e;
    c++;
  }
  return c;
}

// --- Tests ---

void test_basic_spec_structure() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "GET", "/", h, 1, "/", NULL, NULL, "Root",
                            "Root endpoint");

  cJSON* spec =
      csilk_generate_openapi_json(r, "TestAPI", "2.0.0", "Description");
  assert(spec);

  // Check openapi version
  assert(json_has_string(spec, "openapi", "3.0.3"));

  // Check info section
  cJSON* info = cJSON_GetObjectItem(spec, "info");
  assert(info);
  assert(json_has_string(info, "title", "TestAPI"));
  assert(json_has_string(info, "version", "2.0.0"));
  assert(json_has_string(info, "description", "Description"));

  // Check paths
  cJSON* paths = cJSON_GetObjectItem(spec, "paths");
  assert(paths);

  cJSON* get_op = get_path(spec, "get", "/");
  assert(get_op);
  assert(json_has_string(get_op, "summary", "Root"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_basic_spec_structure PASSED\n");
}

void test_path_parameter_conversion() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "GET", "/users/:id", h, 1, "/users/:id", NULL,
                            "Product", "Get User", NULL);
  csilk_router_add_extended(r, "GET", "/files/*path", h, 1, "/files/*path",
                            NULL, NULL, NULL, NULL);

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  // Check :id → {id}
  cJSON* user_get = get_path(spec, "get", "/users/{id}");
  assert(user_get);
  cJSON* params = cJSON_GetObjectItem(user_get, "parameters");
  assert(params && cJSON_IsArray(params));
  assert(count_array(params) == 1);
  cJSON* p = cJSON_GetArrayItem(params, 0);
  assert(p);
  assert(json_has_string(p, "name", "id"));
  assert(json_has_string(p, "in", "path"));

  // Check *path → {path+}
  cJSON* file_get = get_path(spec, "get", "/files/{path+}");
  assert(file_get);
  params = cJSON_GetObjectItem(file_get, "parameters");
  assert(params && cJSON_IsArray(params));
  assert(count_array(params) == 1);
  p = cJSON_GetArrayItem(params, 0);
  assert(json_has_string(p, "name", "path"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_path_parameter_conversion PASSED\n");
}

void test_request_response_types() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "POST", "/login", h, 1, "/login", "LoginReq",
                            "LoginResp", "User Login", "Authenticate user");

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* post_op = get_path(spec, "post", "/login");
  assert(post_op);

  // Check requestBody
  cJSON* req_body = cJSON_GetObjectItem(post_op, "requestBody");
  assert(req_body);
  cJSON* content = cJSON_GetObjectItem(req_body, "content");
  assert(content);
  cJSON* json_content = cJSON_GetObjectItem(content, "application/json");
  assert(json_content);
  cJSON* schema = cJSON_GetObjectItem(json_content, "schema");
  assert(schema);
  assert(json_has_string(schema, "$ref", "#/components/schemas/LoginReq"));

  // Check response 200 schema
  cJSON* resp200 =
      cJSON_GetObjectItem(cJSON_GetObjectItem(post_op, "responses"), "200");
  assert(resp200);
  content = cJSON_GetObjectItem(resp200, "content");
  assert(content);
  json_content = cJSON_GetObjectItem(content, "application/json");
  assert(json_content);
  schema = cJSON_GetObjectItem(json_content, "schema");
  assert(schema);
  assert(json_has_string(schema, "$ref", "#/components/schemas/LoginResp"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_request_response_types PASSED\n");
}

void test_schema_generation() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "POST", "/products", h, 1, "/products",
                            "Product", "Product", NULL, NULL);

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  // Check components/schemas
  cJSON* components = cJSON_GetObjectItem(spec, "components");
  assert(components);
  cJSON* schemas = cJSON_GetObjectItem(components, "schemas");
  assert(schemas);

  cJSON* product_schema = cJSON_GetObjectItem(schemas, "Product");
  assert(product_schema);
  assert(json_has_string(product_schema, "type", "object"));

  // Check properties
  cJSON* props = cJSON_GetObjectItem(product_schema, "properties");
  assert(props);
  assert(cJSON_GetObjectItem(props, "id"));
  assert(cJSON_GetObjectItem(props, "name"));
  assert(cJSON_GetObjectItem(props, "price"));
  assert(cJSON_GetObjectItem(props, "active"));

  // Verify type mapping
  cJSON* name_prop = cJSON_GetObjectItem(props, "name");
  assert(json_has_string(name_prop, "type", "string"));

  cJSON* id_prop = cJSON_GetObjectItem(props, "id");
  assert(json_has_string(id_prop, "type", "integer"));

  cJSON* price_prop = cJSON_GetObjectItem(props, "price");
  assert(json_has_string(price_prop, "type", "number"));

  cJSON* active_prop = cJSON_GetObjectItem(props, "active");
  assert(json_has_string(active_prop, "type", "boolean"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_schema_generation PASSED\n");
}

void test_multiple_methods_same_path() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "GET", "/items", h, 1, "/items", NULL, "Product",
                            "List items", NULL);
  csilk_router_add_extended(r, "POST", "/items", h, 1, "/items", "Product",
                            "Product", "Create item", NULL);
  csilk_router_add_extended(r, "DELETE", "/items/:id", h, 1, "/items/:id", NULL,
                            NULL, "Delete item", NULL);

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* paths = cJSON_GetObjectItem(spec, "paths");
  assert(paths);

  // Check multiple methods on /items
  cJSON* items = cJSON_GetObjectItem(paths, "/items");
  assert(items);
  assert(cJSON_GetObjectItem(items, "get"));
  assert(cJSON_GetObjectItem(items, "post"));

  // Each method should have correct summary
  cJSON* get_op = cJSON_GetObjectItem(items, "get");
  assert(json_has_string(get_op, "summary", "List items"));
  cJSON* post_op = cJSON_GetObjectItem(items, "post");
  assert(json_has_string(post_op, "summary", "Create item"));

  // Check /items/{id}
  cJSON* items_id = cJSON_GetObjectItem(paths, "/items/{id}");
  assert(items_id);
  cJSON* del_op = cJSON_GetObjectItem(items_id, "delete");
  assert(del_op);
  assert(json_has_string(del_op, "summary", "Delete item"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_multiple_methods_same_path PASSED\n");
}

void test_route_without_types() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add(r, "GET", "/plain", h, 1);

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* get_op = get_path(spec, "get", "/plain");
  assert(get_op);

  // Should NOT have requestBody
  assert(cJSON_GetObjectItem(get_op, "requestBody") == NULL);

  // Should have default response (no schema ref)
  cJSON* resp200 =
      cJSON_GetObjectItem(cJSON_GetObjectItem(get_op, "responses"), "200");
  assert(resp200);
  // No content section for type-less routes
  assert(cJSON_GetObjectItem(resp200, "content") == NULL);

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_route_without_types PASSED\n");
}

void test_router_collect_routes() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h1[] = {dummy_handler};
  csilk_router_add_extended(r, "GET", "/a", h1, 1, "/a", NULL, NULL, "A", NULL);
  csilk_handler_t h2[] = {dummy_handler};
  csilk_router_add_extended(r, "POST", "/b", h2, 1, "/b", "LoginReq", NULL,
                            NULL, NULL);

  cJSON* routes = csilk_router_collect_routes(r);
  assert(routes);
  assert(cJSON_IsArray(routes));
  assert(count_array(routes) == 2);

  cJSON* first = cJSON_GetArrayItem(routes, 0);
  assert(first);
  assert(json_has_string(first, "method", "GET"));
  assert(json_has_string(first, "path", "/a"));

  cJSON* second = cJSON_GetArrayItem(routes, 1);
  assert(second);
  assert(json_has_string(second, "method", "POST"));
  assert(json_has_string(second, "path", "/b"));
  assert(json_has_string(second, "input_type", "LoginReq"));

  cJSON_Delete(routes);
  csilk_router_free(r);
  printf("test_router_collect_routes PASSED\n");
}

void test_no_duplicate_schemas() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  // Two routes referencing the same type
  csilk_router_add_extended(r, "GET", "/p1", h, 1, "/p1", NULL, "Product", NULL,
                            NULL);
  csilk_router_add_extended(r, "GET", "/p2", h, 1, "/p2", NULL, "Product", NULL,
                            NULL);

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* schemas =
      cJSON_GetObjectItem(cJSON_GetObjectItem(spec, "components"), "schemas");
  assert(schemas);

  // Product should appear exactly once
  cJSON* product = cJSON_GetObjectItem(schemas, "Product");
  assert(product);

  // Count properties inside product schema
  cJSON* props = cJSON_GetObjectItem(product, "properties");
  int count = 0;
  cJSON* p;
  cJSON_ArrayForEach(p, props) {
    (void)p;
    count++;
  }
  assert(count == 4);  // id, name, price, active

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_no_duplicate_schemas PASSED\n");
}

void test_extended_route_macro() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  CSILK_ROUTE(r, "PUT", "/item/:id", h, 1, "Product", "Product", "Update",
              "Update item");

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* put_op = get_path(spec, "put", "/item/{id}");
  assert(put_op);
  assert(json_has_string(put_op, "summary", "Update"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_extended_route_macro PASSED\n");
}

void test_descriptions_and_summaries() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "GET", "/test", h, 1, "/test", NULL, NULL,
                            "Short", "Long description");

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* op = get_path(spec, "get", "/test");
  assert(op);
  assert(json_has_string(op, "summary", "Short"));
  assert(json_has_string(op, "description", "Long description"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_descriptions_and_summaries PASSED\n");
}

void test_serve_openapi_handler() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended(r, "GET", "/ping", h, 1, "/ping", NULL, NULL,
                            "Ping", NULL);

  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.arena = csilk_arena_new(1024);
  ctx.current_handler = NULL;

  csilk_serve_openapi(&ctx, r, "Served", "1.0.0", "Served API");

  // Should have populated the response
  assert(ctx.response.status == 200);
  assert(ctx.response.body != NULL);
  assert(strstr(ctx.response.body, "\"openapi\"") != NULL);
  assert(strstr(ctx.response.body, "\"Served\"") != NULL);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  csilk_router_free(r);
  printf("test_serve_openapi_handler PASSED\n");
}

void test_serve_openapi_null_router() {
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.arena = csilk_arena_new(1024);

  csilk_serve_openapi(&ctx, NULL, "T", "1", NULL);
  // Should not crash, response should be empty
  assert(ctx.response.status == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("test_serve_openapi_null_router PASSED\n");
}

void test_serve_swagger_ui() {
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.arena = csilk_arena_new(1024);

  csilk_serve_swagger_ui(&ctx);

  assert(ctx.response.status == 200);
  assert(ctx.response.body != NULL);
  assert(strstr(ctx.response.body, "swagger-ui") != NULL);
  assert(strstr(ctx.response.body, "/openapi.json") != NULL);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("test_serve_swagger_ui PASSED\n");
}

void test_serve_swagger_ui_null_ctx() {
  csilk_serve_swagger_ui(NULL);
  printf("test_serve_swagger_ui_null_ctx PASSED\n");
}

void test_auto_register_all_types() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  // Create a route WITHOUT type references - types are registered
  // via CSILK_REGISTER_REFLECT (Product, LoginReq, LoginResp from top of file)
  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add(r, "GET", "/foo", h, 1);

  cJSON* spec = csilk_generate_openapi_json(r, "T", "1", NULL);
  assert(spec);

  cJSON* schemas =
      cJSON_GetObjectItem(cJSON_GetObjectItem(spec, "components"), "schemas");
  assert(schemas);

  // All reflected types should auto-appear in components/schemas
  // even though no route references them
  assert(cJSON_GetObjectItem(schemas, "Product"));
  assert(cJSON_GetObjectItem(schemas, "LoginReq"));
  assert(cJSON_GetObjectItem(schemas, "LoginResp"));

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_auto_register_all_types PASSED\n");
}

void test_empty_router() {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  cJSON* spec = csilk_generate_openapi_json(r, "Empty", "1", NULL);
  assert(spec);

  cJSON* paths = cJSON_GetObjectItem(spec, "paths");
  assert(paths);
  assert(count_array(paths->child) == 0 || cJSON_GetArraySize(paths) == 0);

  cJSON* routes = csilk_router_collect_routes(r);
  assert(routes);
  assert(count_array(routes) == 0);
  cJSON_Delete(routes);

  cJSON_Delete(spec);
  csilk_router_free(r);
  printf("test_empty_router PASSED\n");
}

int main() {
  csilk_reflect_init();

  test_basic_spec_structure();
  test_path_parameter_conversion();
  test_request_response_types();
  test_schema_generation();
  test_multiple_methods_same_path();
  test_route_without_types();
  test_router_collect_routes();
  test_no_duplicate_schemas();
  test_extended_route_macro();
  test_descriptions_and_summaries();
  test_serve_openapi_handler();
  test_serve_openapi_null_router();
  test_serve_swagger_ui();
  test_serve_swagger_ui_null_ctx();
  test_empty_router();
  test_auto_register_all_types();

  printf("\nAll swagger tests PASSED\n");
  return 0;
}
