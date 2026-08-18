/**
 * @file example_rest_api.c
 * @brief Complete REST API with CRUD, validation, pagination, and error handling.
 *
 * Demonstrates a production-style REST API using csilk's routing, JSON binding,
 * query parsing, and middleware stack.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"

/* ====================================================================
 *  In-memory "database" — simulates a persistent store
 * ==================================================================== */

#define MAX_ITEMS 100

typedef struct {
    int64_t id;
    char    name[128];
    char    email[128];
    int     active;
} Item;

static Item    g_items[MAX_ITEMS];
static int     g_item_count = 0;
static int64_t g_next_id = 1;

static void
init_sample_data(void)
{
    const char* names[] = {"Alice", "Bob", "Charlie", "Diana", "Eve"};
    for (int i = 0; i < 5 && g_item_count < MAX_ITEMS; i++) {
        Item* item = &g_items[g_item_count++];
        item->id = g_next_id++;
        snprintf(item->name, sizeof(item->name), "%s", names[i]);
        snprintf(item->email, sizeof(item->email), "%s@example.com", names[i]);
        item->active = 1;
    }
}

/* ====================================================================
 *  Helper: build paginated JSON response
 * ==================================================================== */

static void
send_paginated(csilk_ctx_t* c, Item* items, int count, int total, int page, int per_page)
{
    /* Build items array using csilk_json API */
    csilk_json_t* arr = csilk_json_array();
    for (int i = 0; i < count; i++) {
        csilk_json_t* obj = csilk_json_object();
        csilk_json_add_int(obj, "id", items[i].id);
        csilk_json_add_string(obj, "name", items[i].name);
        csilk_json_add_string(obj, "email", items[i].email);
        csilk_json_add_bool(obj, "active", items[i].active);
        csilk_json_array_append(arr, obj);
    }

    csilk_json_t* resp = csilk_json_object();
    csilk_json_add_array(resp, "items", arr);
    csilk_json_add_int(resp, "total", total);
    csilk_json_add_int(resp, "page", page);
    csilk_json_add_int(resp, "per_page", per_page);
    csilk_json_add_int(resp, "total_pages", (total + per_page - 1) / per_page);
    csilk_json_add_bool(resp, "has_next", (per_page - count) > 0);
    csilk_json_add_bool(resp, "has_prev", page > 1);
    csilk_json(c, 200, resp);
}

/* ====================================================================
 *  CRUD Handlers
 * ==================================================================== */

static void
list_items_handler(csilk_ctx_t* c)
{
    /* Parse query params: page, per_page, active */
    const char* page_str = csilk_get_query(c, "page");
    const char* limit_str = csilk_get_query(c, "per_page");
    const char* active_str = csilk_get_query(c, "active");

    int page = page_str ? atoi(page_str) : 1;
    int per_page = limit_str ? atoi(limit_str) : 10;
    int show_active = active_str ? atoi(active_str) : -1; /* -1 = all */

    if (page < 1) {
        page = 1;
    }
    if (per_page < 1 || per_page > 100) {
        per_page = 10;
    }

    /* Apply filter and compute offset */
    int total = 0;
    for (int i = 0; i < g_item_count; i++) {
        if (show_active >= 0 && g_items[i].active != show_active) {
            continue;
        }
        total++;
    }

    int  offset = (page - 1) * per_page;
    int  count = 0;
    Item slice[MAX_ITEMS];

    for (int i = 0; i < g_item_count && count < per_page; i++) {
        if (show_active >= 0 && g_items[i].active != show_active) {
            continue;
        }
        if (offset > 0) {
            offset--;
            continue;
        }
        slice[count++] = g_items[i];
    }

    send_paginated(c, slice, count, total, page, per_page);
}

static void
get_item_handler(csilk_ctx_t* c)
{
    const char* id_str = csilk_get_param(c, "id");
    if (!id_str) {
        csilk_json_error(c, 400, "id parameter required");
        return;
    }

    int64_t id = atoll(id_str);
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].id == id) {
            csilk_json_t* obj = csilk_json_object();
            csilk_json_add_int(obj, "id", g_items[i].id);
            csilk_json_add_string(obj, "name", g_items[i].name);
            csilk_json_add_string(obj, "email", g_items[i].email);
            csilk_json_add_bool(obj, "active", g_items[i].active);
            csilk_json(c, 200, obj);
            return;
        }
    }
    csilk_json_error(c, 404, "item not found");
}

static void
create_item_handler(csilk_ctx_t* c)
{
    if (g_item_count >= MAX_ITEMS) {
        csilk_json_error(c, 413, "max items reached");
        return;
    }

    csilk_json_t* body = csilk_bind_json(c);
    if (!body) {
        csilk_json_error(c, 400, "invalid json body");
        return;
    }

    const char* name = csilk_json_get_string(body, "name");
    const char* email = csilk_json_get_string(body, "email");

    if (!name || strlen(name) == 0) {
        csilk_json_error(c, 400, "name is required");
        return;
    }
    if (!email || strlen(email) == 0) {
        csilk_json_error(c, 400, "email is required");
        return;
    }

    Item* item = &g_items[g_item_count++];
    item->id = g_next_id++;
    snprintf(item->name, sizeof(item->name), "%s", name);
    snprintf(item->email, sizeof(item->email), "%s", email);
    item->active = 1;

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "id", item->id);
    csilk_json_add_string(obj, "name", item->name);
    csilk_json_add_string(obj, "email", item->email);
    csilk_json_add_bool(obj, "active", true);
    csilk_json_add_bool(obj, "created", true);
    csilk_json(c, 201, obj);
}

static void
update_item_handler(csilk_ctx_t* c)
{
    const char* id_str = csilk_get_param(c, "id");
    if (!id_str) {
        csilk_json_error(c, 400, "id parameter required");
        return;
    }

    int64_t id = atoll(id_str);
    Item*   item = NULL;
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].id == id) {
            item = &g_items[i];
            break;
        }
    }
    if (!item) {
        csilk_json_error(c, 404, "item not found");
        return;
    }

    csilk_json_t* body = csilk_bind_json(c);
    if (body) {
        const char* name = csilk_json_get_string(body, "name");
        const char* email = csilk_json_get_string(body, "email");
        if (name) {
            snprintf(item->name, sizeof(item->name), "%s", name);
        }
        if (email) {
            snprintf(item->email, sizeof(item->email), "%s", email);
        }
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "id", item->id);
    csilk_json_add_string(obj, "name", item->name);
    csilk_json_add_string(obj, "email", item->email);
    csilk_json_add_bool(obj, "active", item->active);
    csilk_json_add_bool(obj, "updated", true);
    csilk_json(c, 200, obj);
}

static void
delete_item_handler(csilk_ctx_t* c)
{
    const char* id_str = csilk_get_param(c, "id");
    if (!id_str) {
        csilk_json_error(c, 400, "id parameter required");
        return;
    }

    int64_t id = atoll(id_str);
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].id == id) {
            /* Swap with last and shrink */
            g_items[i] = g_items[--g_item_count];
            csilk_json_t* obj = csilk_json_object();
            csilk_json_add_int(obj, "id", id);
            csilk_json_add_bool(obj, "deleted", true);
            csilk_json(c, 200, obj);
            return;
        }
    }
    csilk_json_error(c, 404, "item not found");
}

static void
toggle_item_handler(csilk_ctx_t* c)
{
    const char* id_str = csilk_get_param(c, "id");
    if (!id_str) {
        csilk_json_error(c, 400, "id parameter required");
        return;
    }

    int64_t id = atoll(id_str);
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].id == id) {
            g_items[i].active = !g_items[i].active;
            csilk_json_t* obj = csilk_json_object();
            csilk_json_add_int(obj, "id", id);
            csilk_json_add_bool(obj, "active", g_items[i].active);
            csilk_json_add_bool(obj, "toggled", true);
            csilk_json(c, 200, obj);
            return;
        }
    }
    csilk_json_error(c, 404, "item not found");
}

static void
count_handler(csilk_ctx_t* c)
{
    (void)c;
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "total", g_item_count);
    csilk_json(c, 200, obj);
}

static void
health_handler(csilk_ctx_t* c)
{
    (void)c;
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "status", "ok");
    csilk_json_add_int(obj, "items", g_item_count);
    csilk_json(c, 200, obj);
}

/* ====================================================================
 *  Main
 * ==================================================================== */

int
main(void)
{
    init_sample_data();

    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    /* Middleware stack */
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);
    csilk_group_use(root, csilk_request_id_middleware);
    csilk_group_use(root, csilk_gzip_middleware);

    /* API group with version prefix — all groups are created from the router */
    csilk_group_t* api = csilk_group_new(router, "/api/v1");

    /* CRUD routes */
    csilk_GET(api, "/items", list_items_handler);
    csilk_GET(api, "/items/:id", get_item_handler);
    csilk_POST(api, "/items", create_item_handler);
    csilk_PUT(api, "/items/:id", update_item_handler);
    csilk_DELETE(api, "/items/:id", delete_item_handler);

    /* Utility routes */
    csilk_PATCH(api, "/items/:id/toggle", toggle_item_handler);
    csilk_GET(api, "/items/count", count_handler);
    csilk_GET(root, "/health", health_handler);

    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("\n=== REST API Example ===\n");
    printf("GET    /api/v1/items?page=1&per_page=10&active=1\n");
    printf("GET    /api/v1/items/:id\n");
    printf("POST   /api/v1/items        Body: {\"name\":\"...\",\"email\":\"...\"}\n");
    printf("PUT    /api/v1/items/:id    Body: {\"name\":\"...\",\"email\":\"...\"}\n");
    printf("DELETE /api/v1/items/:id\n");
    printf("PATCH  /api/v1/items/:id/toggle\n");
    printf("GET    /health\n");
    printf("Listen: http://localhost:8080\n");

    csilk_server_run(server, 8080);

    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);
    return 0;
}
