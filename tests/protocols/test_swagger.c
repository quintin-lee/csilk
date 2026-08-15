#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"
#include "csilk/test/test.h"

// Test structs for reflection-based schema generation
typedef struct {
    int32_t id;
    char    name[64];
    double  price;
    bool    active;
} Product;

typedef struct {
    char username[64];
    char password[64];
} LoginReq;

typedef struct {
    char token[128];
    char message[256];
} LoginResp;

#define PRODUCT_MAP(X)                                                                             \
    X(Product, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr)                           \
    X(Product, name, CSILK_TYPE_STRING, sizeof(((Product*)0)->name), 0, false, nullptr)            \
    X(Product, price, CSILK_TYPE_DOUBLE, sizeof(double), 0, false, nullptr)                        \
    X(Product, active, CSILK_TYPE_BOOL, sizeof(bool), 0, false, nullptr)

#define LOGINREQ_MAP(X)                                                                            \
    X(LoginReq, username, CSILK_TYPE_STRING, sizeof(((LoginReq*)0)->username), 0, false, nullptr)  \
    X(LoginReq, password, CSILK_TYPE_STRING, sizeof(((LoginReq*)0)->password), 0, false, nullptr)

#define LOGINRESP_MAP(X)                                                                           \
    X(LoginResp, token, CSILK_TYPE_STRING, sizeof(((LoginResp*)0)->token), 0, false, nullptr)      \
    X(LoginResp, message, CSILK_TYPE_STRING, sizeof(((LoginResp*)0)->message), 0, false, nullptr)

CSILK_REGISTER_REFLECT(Product, PRODUCT_MAP)
CSILK_REGISTER_REFLECT(LoginReq, LOGINREQ_MAP)
CSILK_REGISTER_REFLECT(LoginResp, LOGINRESP_MAP)

// Dummy handlers
void
dummy_handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "ok");
}

// --- Helper to verify cJSON tree ---

static int
json_has_string(csilk_json_t* obj, const char* key, const char* val)
{
    csilk_json_t* item = csilk_json_get(obj, key);
    return item && csilk_json_is_string(item) && strcmp(csilk_json_string_value(item), val) == 0;
}

static csilk_json_t*
get_path(csilk_json_t* spec, const char* method, const char* path)
{
    csilk_json_t* paths = csilk_json_get(spec, "paths");
    if (!paths) {
        return nullptr;
    }
    csilk_json_t* pobj = csilk_json_get(paths, path);
    if (!pobj) {
        return nullptr;
    }
    return csilk_json_get(pobj, method);
}

static int
count_array(csilk_json_t* arr)
{
    if (!arr || !csilk_json_is_array(arr)) {
        return 0;
    }
    int c = 0;
    for (size_t _i = 0; _i < csilk_json_array_size(arr); _i++) {
        csilk_json_t* e = csilk_json_array_get(arr, _i);
        if (!e) {
            break;
        }
        c++;
    }
    return c;
}

// --- Tests ---

void
test_basic_spec_structure()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(r, "GET", "/", h, 1, "/", nullptr, nullptr, "Root", "Root endpoint");

    csilk_json_t* spec = csilk_generate_openapi_json(r, "TestAPI", "2.0.0", "Description");
    assert(spec);

    // Check openapi version
    assert(json_has_string(spec, "openapi", "3.0.3"));

    // Check info section
    csilk_json_t* info = csilk_json_get(spec, "info");
    assert(info);
    assert(json_has_string(info, "title", "TestAPI"));
    assert(json_has_string(info, "version", "2.0.0"));
    assert(json_has_string(info, "description", "Description"));

    // Check paths
    csilk_json_t* paths = csilk_json_get(spec, "paths");
    assert(paths);

    csilk_json_t* get_op = get_path(spec, "get", "/");
    assert(get_op);
    assert(json_has_string(get_op, "summary", "Root"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_basic_spec_structure PASSED\n");
}

void
test_path_parameter_conversion()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(
        r, "GET", "/users/:id", h, 1, "/users/:id", nullptr, "Product", "Get User", nullptr);
    csilk_router_add_extended(
        r, "GET", "/files/*path", h, 1, "/files/*path", nullptr, nullptr, nullptr, nullptr);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    // Check :id → {id}
    csilk_json_t* user_get = get_path(spec, "get", "/users/{id}");
    assert(user_get);
    csilk_json_t* params = csilk_json_get(user_get, "parameters");
    assert(params && csilk_json_is_array(params));
    assert(count_array(params) == 1);
    csilk_json_t* p = csilk_json_array_get(params, 0);
    assert(p);
    assert(json_has_string(p, "name", "id"));
    assert(json_has_string(p, "in", "path"));

    // Check *path → {path+}
    csilk_json_t* file_get = get_path(spec, "get", "/files/{path+}");
    assert(file_get);
    params = csilk_json_get(file_get, "parameters");
    assert(params && csilk_json_is_array(params));
    assert(count_array(params) == 1);
    p = csilk_json_array_get(params, 0);
    assert(json_has_string(p, "name", "path"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_path_parameter_conversion PASSED\n");
}

void
test_request_response_types()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(r,
                              "POST",
                              "/login",
                              h,
                              1,
                              "/login",
                              "LoginReq",
                              "LoginResp",
                              "User Login",
                              "Authenticate user");

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* post_op = get_path(spec, "post", "/login");
    assert(post_op);

    // Check requestBody
    csilk_json_t* req_body = csilk_json_get(post_op, "requestBody");
    assert(req_body);
    csilk_json_t* content = csilk_json_get(req_body, "content");
    assert(content);
    csilk_json_t* json_content = csilk_json_get(content, "application/json");
    assert(json_content);
    csilk_json_t* schema = csilk_json_get(json_content, "schema");
    assert(schema);
    assert(json_has_string(schema, "$ref", "#/components/schemas/LoginReq"));

    // Check response 200 schema
    csilk_json_t* resp200 = csilk_json_get(csilk_json_get(post_op, "responses"), "200");
    assert(resp200);
    content = csilk_json_get(resp200, "content");
    assert(content);
    json_content = csilk_json_get(content, "application/json");
    assert(json_content);
    schema = csilk_json_get(json_content, "schema");
    assert(schema);
    assert(json_has_string(schema, "$ref", "#/components/schemas/LoginResp"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_request_response_types PASSED\n");
}

void
test_schema_generation()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(
        r, "POST", "/products", h, 1, "/products", "Product", "Product", nullptr, nullptr);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    // Check components/schemas
    csilk_json_t* components = csilk_json_get(spec, "components");
    assert(components);
    csilk_json_t* schemas = csilk_json_get(components, "schemas");
    assert(schemas);

    csilk_json_t* product_schema = csilk_json_get(schemas, "Product");
    assert(product_schema);
    assert(json_has_string(product_schema, "type", "object"));

    // Check properties
    csilk_json_t* props = csilk_json_get(product_schema, "properties");
    assert(props);
    assert(csilk_json_get(props, "id"));
    assert(csilk_json_get(props, "name"));
    assert(csilk_json_get(props, "price"));
    assert(csilk_json_get(props, "active"));

    // Verify type mapping
    csilk_json_t* name_prop = csilk_json_get(props, "name");
    assert(json_has_string(name_prop, "type", "string"));

    csilk_json_t* id_prop = csilk_json_get(props, "id");
    assert(json_has_string(id_prop, "type", "integer"));

    csilk_json_t* price_prop = csilk_json_get(props, "price");
    assert(json_has_string(price_prop, "type", "number"));

    csilk_json_t* active_prop = csilk_json_get(props, "active");
    assert(json_has_string(active_prop, "type", "boolean"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_schema_generation PASSED\n");
}

void
test_multiple_methods_same_path()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(
        r, "GET", "/items", h, 1, "/items", nullptr, "Product", "List items", nullptr);
    csilk_router_add_extended(
        r, "POST", "/items", h, 1, "/items", "Product", "Product", "Create item", nullptr);
    csilk_router_add_extended(
        r, "DELETE", "/items/:id", h, 1, "/items/:id", nullptr, nullptr, "Delete item", nullptr);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* paths = csilk_json_get(spec, "paths");
    assert(paths);

    // Check multiple methods on /items
    csilk_json_t* items = csilk_json_get(paths, "/items");
    assert(items);
    assert(csilk_json_get(items, "get"));
    assert(csilk_json_get(items, "post"));

    // Each method should have correct summary
    csilk_json_t* get_op = csilk_json_get(items, "get");
    assert(json_has_string(get_op, "summary", "List items"));
    csilk_json_t* post_op = csilk_json_get(items, "post");
    assert(json_has_string(post_op, "summary", "Create item"));

    // Check /items/{id}
    csilk_json_t* items_id = csilk_json_get(paths, "/items/{id}");
    assert(items_id);
    csilk_json_t* del_op = csilk_json_get(items_id, "delete");
    assert(del_op);
    assert(json_has_string(del_op, "summary", "Delete item"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_multiple_methods_same_path PASSED\n");
}

void
test_route_without_types()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add(r, "GET", "/plain", h, 1);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* get_op = get_path(spec, "get", "/plain");
    assert(get_op);

    // Should NOT have requestBody
    assert(csilk_json_get(get_op, "requestBody") == nullptr);

    // Should have default response (no schema ref)
    csilk_json_t* resp200 = csilk_json_get(csilk_json_get(get_op, "responses"), "200");
    assert(resp200);
    // No content section for type-less routes
    assert(csilk_json_get(resp200, "content") == nullptr);

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_route_without_types PASSED\n");
}

void
test_router_collect_routes()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h1[] = {dummy_handler};
    csilk_router_add_extended(r, "GET", "/a", h1, 1, "/a", nullptr, nullptr, "A", nullptr);
    csilk_handler_t h2[] = {dummy_handler};
    csilk_router_add_extended(r, "POST", "/b", h2, 1, "/b", "LoginReq", nullptr, nullptr, nullptr);

    csilk_json_t* routes = csilk_router_collect_routes(r);
    assert(routes);
    assert(csilk_json_is_array(routes));
    assert(count_array(routes) == 2);

    csilk_json_t* first = csilk_json_array_get(routes, 0);
    assert(first);
    assert(json_has_string(first, "method", "GET"));
    assert(json_has_string(first, "path", "/a"));

    csilk_json_t* second = csilk_json_array_get(routes, 1);
    assert(second);
    assert(json_has_string(second, "method", "POST"));
    assert(json_has_string(second, "path", "/b"));
    assert(json_has_string(second, "input_type", "LoginReq"));

    csilk_json_free(routes);
    csilk_router_free(r);
    printf("test_router_collect_routes PASSED\n");
}

void
test_no_duplicate_schemas()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    // Two routes referencing the same type
    csilk_router_add_extended(r, "GET", "/p1", h, 1, "/p1", nullptr, "Product", nullptr, nullptr);
    csilk_router_add_extended(r, "GET", "/p2", h, 1, "/p2", nullptr, "Product", nullptr, nullptr);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* schemas = csilk_json_get(csilk_json_get(spec, "components"), "schemas");
    assert(schemas);

    // Product should appear exactly once
    csilk_json_t* product = csilk_json_get(schemas, "Product");
    assert(product);

    // Count properties inside product schema
    csilk_json_t* props = csilk_json_get(product, "properties");
    int           count = 0;
    for (size_t _i = 0; _i < csilk_json_object_size(props); _i++) {
        csilk_json_t* p = csilk_json_object_val(props, _i);
        if (!p) {
            break;
        }
        count++;
    }
    assert(count == 4); // id, name, price, active

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_no_duplicate_schemas PASSED\n");
}

void
test_extended_route_macro()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    CSILK_ROUTE(r, "PUT", "/item/:id", h, 1, "Product", "Product", "Update", "Update item");

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* put_op = get_path(spec, "put", "/item/{id}");
    assert(put_op);
    assert(json_has_string(put_op, "summary", "Update"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_extended_route_macro PASSED\n");
}

void
test_descriptions_and_summaries()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(
        r, "GET", "/test", h, 1, "/test", nullptr, nullptr, "Short", "Long description");

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* op = get_path(spec, "get", "/test");
    assert(op);
    assert(json_has_string(op, "summary", "Short"));
    assert(json_has_string(op, "description", "Long description"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_descriptions_and_summaries PASSED\n");
}

void
test_serve_openapi_handler()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add_extended(r, "GET", "/ping", h, 1, "/ping", nullptr, nullptr, "Ping", nullptr);

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_serve_openapi(ctx, r, "Served", "1.0.0", "Served API");

    // Should have populated the response
    assert(csilk_get_status(ctx) == 200);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr);
    assert(strstr(body, "\"openapi\"") != nullptr);
    assert(strstr(body, "\"Served\"") != nullptr);

    csilk_test_ctx_free(ctx);
    csilk_router_free(r);
    printf("test_serve_openapi_handler PASSED\n");
}

void
test_serve_openapi_null_router()
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_serve_openapi(ctx, nullptr, "T", "1", nullptr);
    // Should not crash, response should be empty
    assert(csilk_get_status(ctx) == 200 || csilk_get_status(ctx) == 0);

    csilk_test_ctx_free(ctx);
    printf("test_serve_openapi_null_router PASSED\n");
}

void
test_serve_swagger_ui()
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_serve_swagger_ui(ctx);

    assert(csilk_get_status(ctx) == 200);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr);
    assert(strstr(body, "swagger-ui") != nullptr);
    assert(strstr(body, "/openapi.json") != nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_serve_swagger_ui PASSED\n");
}

void
test_serve_swagger_ui_ext()
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_serve_swagger_ui_ext(ctx, "/v2/api-docs");

    assert(csilk_get_status(ctx) == 200);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr);
    assert(strstr(body, "/v2/api-docs") != nullptr);
    assert(strstr(body, "unpkg.com/swagger-ui-dist") != nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_serve_swagger_ui_ext PASSED\n");
}

void
test_serve_swagger_ui_null_ctx()
{
    csilk_serve_swagger_ui(nullptr);
    csilk_serve_swagger_ui_ext(nullptr, "/custom.json");
    printf("test_serve_swagger_ui_null_ctx PASSED\n");
}

void
test_serve_openapi_dynamic_routes()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add(r, "GET", "/first", h, 1);

    csilk_ctx_t* ctx1 = csilk_test_ctx_new();
    csilk_serve_openapi(ctx1, r, "Dynamic API", "1.0.0", "Dynamic description");
    assert(csilk_get_status(ctx1) == 200);
    size_t      len1 = 0;
    const char* body1 = csilk_get_response_body(ctx1, &len1);
    assert(body1 != nullptr);
    assert(strstr(body1, "/first") != nullptr);
    assert(strstr(body1, "/second") == nullptr);
    csilk_test_ctx_free(ctx1);

    /* Add route dynamically after first serve */
    csilk_router_add(r, "POST", "/second", h, 1);

    csilk_ctx_t* ctx2 = csilk_test_ctx_new();
    csilk_serve_openapi(ctx2, r, "Dynamic API", "1.0.0", "Dynamic description");
    assert(csilk_get_status(ctx2) == 200);
    size_t      len2 = 0;
    const char* body2 = csilk_get_response_body(ctx2, &len2);
    assert(body2 != nullptr);
    assert(strstr(body2, "/first") != nullptr);
    assert(strstr(body2, "/second") != nullptr);
    csilk_test_ctx_free(ctx2);

    csilk_router_free(r);
    printf("test_serve_openapi_dynamic_routes PASSED\n");
}

void
test_group_extended_macros()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_group_t* g = csilk_group_new(r, "/api/v1");
    assert(g);

    csilk_GET_EXT(g, "/items", dummy_handler, nullptr, "Product", "List items", "Lists all items");
    csilk_POST_EXT(
        g, "/items", dummy_handler, "Product", "Product", "Create item", "Creates an item");

    csilk_json_t* spec = csilk_generate_openapi_json(r, "API", "1.0", "Group API");
    assert(spec);

    csilk_json_t* get_op = get_path(spec, "get", "/api/v1/items");
    assert(get_op);
    assert(json_has_string(get_op, "summary", "List items"));

    csilk_json_t* post_op = get_path(spec, "post", "/api/v1/items");
    assert(post_op);
    assert(json_has_string(post_op, "summary", "Create item"));

    csilk_json_free(spec);
    csilk_group_free(g);
    csilk_router_free(r);
    printf("test_group_extended_macros PASSED\n");
}

void
test_auto_register_all_types()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    // Create a route WITHOUT type references - types are registered
    // via CSILK_REGISTER_REFLECT (Product, LoginReq, LoginResp from top of file)
    csilk_handler_t h[] = {dummy_handler};
    csilk_router_add(r, "GET", "/foo", h, 1);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "T", "1", nullptr);
    assert(spec);

    csilk_json_t* schemas = csilk_json_get(csilk_json_get(spec, "components"), "schemas");
    assert(schemas);

    // All reflected types should auto-appear in components/schemas
    // even though no route references them
    assert(csilk_json_get(schemas, "Product"));
    assert(csilk_json_get(schemas, "LoginReq"));
    assert(csilk_json_get(schemas, "LoginResp"));

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_auto_register_all_types PASSED\n");
}

void
test_empty_router()
{
    csilk_router_t* r = csilk_router_new();
    assert(r);

    csilk_json_t* spec = csilk_generate_openapi_json(r, "Empty", "1", nullptr);
    assert(spec);

    csilk_json_t* paths = csilk_json_get(spec, "paths");
    assert(paths);
    assert(count_array(paths) == 0);

    csilk_json_t* routes = csilk_router_collect_routes(r);
    assert(routes);
    assert(count_array(routes) == 0);
    csilk_json_free(routes);

    csilk_json_free(spec);
    csilk_router_free(r);
    printf("test_empty_router PASSED\n");
}

int
main()
{
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
    test_serve_openapi_dynamic_routes();
    test_group_extended_macros();
    test_serve_swagger_ui();
    test_serve_swagger_ui_ext();
    test_serve_swagger_ui_null_ctx();
    test_empty_router();
    test_auto_register_all_types();

    printf("\nAll swagger tests PASSED\n");
    return 0;
}
