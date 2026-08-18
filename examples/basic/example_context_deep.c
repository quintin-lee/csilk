/**
 * @file example_context_deep.c
 * @brief Deep context usage: RAII storage, deferred cleanup, panic recovery,
 *        arena allocation, and view-based accessors.
 *
 * Demonstrates proper lifecycle management of per-request resources in csilk.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/csilk.h"

/* ====================================================================
 *  Custom resource type — demonstrates csilk_set_ex with destructor
 * ==================================================================== */

typedef struct {
    char    label[64];
    int64_t created_at;
} demo_resource_t;

static void
demo_resource_free(void* ptr)
{
    demo_resource_t* r = (demo_resource_t*)ptr;
    printf("[dtor] freed demo_resource '%s'\n", r->label);
}

/* ====================================================================
 *  Handlers
 * ==================================================================== */

/**
 * @brief Demonstrates csilk_set_ex / csilk_get with arena-backed values.
 *
 * Resources stored via csilk_set_ex are automatically freed when:
 *   - The value is overwritten
 *   - The context is cleaned up (arena reset)
 *   - A panic recovery runs defer_free()
 */
static void
raii_storage_handler(csilk_ctx_t* c)
{
    /* Allocate on the arena — zero malloc overhead per request */
    csilk_arena_t*   arena = csilk_get_arena(c);
    demo_resource_t* res = (demo_resource_t*)csilk_arena_alloc(arena, sizeof(demo_resource_t));
    if (!res) {
        csilk_json_error(c, 500, "OOM");
        return;
    }
    const char* rid = csilk_get_request_id(c);
    snprintf(res->label, sizeof(res->label), "resource-%s", rid ? rid : "0");
    res->created_at = (int64_t)time(NULL);

    /* Register with automatic cleanup via csilk_set_ex */
    csilk_set_ex(c, "my_resource", res, demo_resource_free);

    /* Retrieve it back — same pointer, no extra lookup cost */
    demo_resource_t* retrieved = (demo_resource_t*)csilk_get(c, "my_resource");
    if (!retrieved) {
        csilk_json_error(c, 500, "storage lost");
        return;
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "label", retrieved->label);
    csilk_json_add_int(obj, "created_at", retrieved->created_at);
    csilk_json(c, 200, obj);
}

/**
 * @brief Demonstrates csilk_ctx_defer for panic-safe resource cleanup.
 *
 * Deferred callbacks run in LIFO order when:
 *   - csilk_ctx_cleanup() is called normally
 *   - csilk_panic() triggers recovery
 *   This is critical for file descriptors, mutexes, and heap memory.
 */
static void
defer_cleanup_handler(csilk_ctx_t* c)
{
    /* Simulate opening a file or acquiring a lock */
    int* fd_ptr = (int*)malloc(sizeof(int));
    if (!fd_ptr) {
        csilk_json_error(c, 500, "OOM");
        return;
    }
    *fd_ptr = 42; /* fake fd */

    /* Register deferred cleanup — runs on panic OR normal cleanup */
    csilk_ctx_defer(c, free, fd_ptr);

    /* Also demonstrate arena-allocated data with defer */
    csilk_arena_t* arena = csilk_get_arena(c);
    char*          msg = (char*)csilk_arena_strndup(arena, "deferred message", 16);
    if (msg) {
        csilk_ctx_defer(c, NULL, msg); /* NULL fn = just mark for tracking */
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "fd_hint", *fd_ptr);
    csilk_json_add_string(obj, "message", "defer registered");
    csilk_json(c, 200, obj);
}

/**
 * @brief Demonstrates csilk_panic recovery flow.
 *
 * The handler intentionally calls csilk_panic to show how the recovery
 * middleware catches it and returns a clean 500 response.
 */
static void
panic_demo_handler(csilk_ctx_t* c)
{
    /* Pre-allocate a resource that MUST be cleaned up on panic */
    char* leak_prone = (char*)malloc(1024);
    if (leak_prone) {
        csilk_ctx_defer(c, free, leak_prone);
    }

    printf("[demo] about to trigger panic...\n");
    csilk_panic(c); /* sets panicked=1, aborted=1, runs defer_free */

    /* This line is NEVER reached — chain is aborted above */
    csilk_json(c, 200, csilk_json_object());
}

/**
 * @brief Demonstrates view-based zero-copy accessors.
 *
 * csilk_get_param_view / csilk_get_query_view return csilk_view_t —
 * a (const char*, size_t) pair that points directly into parser buffers
 * without any malloc. This is the preferred pattern for read access.
 */
static void
view_access_handler(csilk_ctx_t* c)
{
    /* Path param as view (zero-copy) */
    csilk_view_t id_view = csilk_get_param_view(c, "id");
    if (id_view.data && id_view.len > 0) {
        csilk_json_t* obj = csilk_json_object();
        csilk_json_add_string(obj, "param_id", id_view.data);
        csilk_json_add_int(obj, "param_len", (int64_t)id_view.len);
        csilk_json(c, 200, obj);
        return;
    }

    /* Query param as view (zero-copy) */
    csilk_view_t q = csilk_get_query_view(c, "q");
    if (q.data && q.len > 0) {
        csilk_json_t* obj = csilk_json_object();
        csilk_json_add_string(obj, "query", q.data);
        csilk_json_add_int(obj, "len", (int64_t)q.len);
        csilk_json(c, 200, obj);
        return;
    }

    /* Header as view (zero-copy) */
    csilk_view_t ua = csilk_get_header_view(c, "User-Agent");
    if (ua.data && ua.len > 0) {
        csilk_json_t* obj = csilk_json_object();
        csilk_json_add_string(obj, "user_agent", ua.data);
        csilk_json_add_int(obj, "ua_len", (int64_t)ua.len);
        csilk_json(c, 200, obj);
        return;
    }

    csilk_json_error(c, 400, "try ?q=hello or path /:id or User-Agent header");
}

/**
 * @brief Demonstrates body ownership modes with zero-copy views.
 *
 * The request body can be:
 *   - BORROWED (zero-copy reference into recv buffer)
 *   - OWNED_HEAP (copied to heap for async processing)
 *   - OWNED_ARENA (copied to request arena)
 */
static void
body_modes_handler(csilk_ctx_t* c)
{
    /* Zero-copy body view — no malloc, points into parser buffer */
    csilk_view_t body_view = csilk_get_body_view(c);

    if (body_view.len == 0) {
        csilk_json_error(c, 400, "empty body");
        return;
    }

    /* Demonstrate copying body to arena for persistence across requests */
    csilk_arena_t* arena = csilk_get_arena(c);
    char*          arena_copy = csilk_arena_strndup(arena, body_view.data, body_view.len);
    if (!arena_copy) {
        csilk_json_error(c, 500, "OOM");
        return;
    }

    size_t        preview_len = body_view.len < 64 ? body_view.len : 64;
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "body_len", (int64_t)body_view.len);
    csilk_json_add_string(obj, "body_preview", arena_copy);
    csilk_json_add_bool(obj, "arena_copy_ok", true);
    csilk_json(c, 200, obj);
}

/* ====================================================================
 *  Main
 * ==================================================================== */

int
main(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    /* Global middleware — panic recovery MUST be first */
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);

    /* Demo handlers */
    csilk_GET(root, "/raii", raii_storage_handler);
    csilk_GET(root, "/defer", defer_cleanup_handler);
    csilk_GET(root, "/panic-demo", panic_demo_handler);
    csilk_GET(root, "/view/:id", view_access_handler);
    csilk_GET(root, "/view", view_access_handler);
    csilk_POST(root, "/body-modes", body_modes_handler);

    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("\n=== Context Deep Dive Example ===\n");
    printf("GET  /raii          — RAII storage (csilk_set_ex + arena)\n");
    printf("GET  /defer         — Deferred cleanup (csilk_ctx_defer)\n");
    printf("GET  /panic-demo    — Panic recovery + leak prevention\n");
    printf("GET  /view/:id      — Zero-copy view accessors (params, query, headers)\n");
    printf("POST /body-modes    — Body ownership and zero-copy views\n");
    printf("Listen: http://localhost:8080\n");

    csilk_server_run(server, 8080);

    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);
    return 0;
}
