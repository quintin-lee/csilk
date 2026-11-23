#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

extern csilk_arena_t*  pool_get_arena(worker_pool_t* wp);
extern void            pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena);
extern void            _csilk_handle_post_response(csilk_client_t* client, int keep_alive);
extern csilk_client_t* pool_get(worker_pool_t* wp);
extern void            pool_put(worker_pool_t* wp, csilk_client_t* client);

static csilk_server_t*
mock_server(csilk_io_loop_t* loop)
{
    csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
    s->loop = loop;
    s->worker_pools = calloc(1, sizeof(worker_pool_t));
    _csilk_worker_pool_atomics_init(&s->worker_pools[0], s, 0);
    s->worker_pool_count = 1;
    return s;
}

static void
free_mock_server(csilk_server_t* s)
{
    free(s->worker_pools);
    free(s);
}

static void
test_lazy_arena_lifecycle(void)
{
    printf("Testing lazy arena allocation and keep-alive recycling...\n");

    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    csilk_server_t* server = mock_server(&loop);
    assert(server != nullptr);

    worker_pool_t* wp = &server->worker_pools[0];

    /* 1. Allocate client from pool */
    csilk_client_t* client = pool_get(wp);
    assert(client != nullptr);
    client->server = server;
    client->owner_pool = wp;

    csilk_io_timer_init(&loop, &client->timer);
    csilk_io_timer_init(&loop, &client->read_timer);
    csilk_io_timer_init(&loop, &client->write_timer);
    csilk_io_timer_init(&loop, &client->request_timer);

    /* Initialize ctx without preallocating arena (lazy mode) */
    _csilk_ctx_init(&client->ctx, server, client);
    client->ctx.arena = nullptr;

    /* Verify lazy: initial arena is NULL */
    assert(client->ctx.arena == nullptr);

    /* 2. On-demand acquisition via csilk_get_arena */
    csilk_arena_t* arena = csilk_get_arena(&client->ctx);
    assert(arena != nullptr);
    assert(client->ctx.arena == arena);

    /* Use arena for an allocation */
    char* test_str = csilk_arena_strdup(arena, "csilk lazy arena");
    assert(test_str != nullptr);
    assert(strcmp(test_str, "csilk lazy arena") == 0);

    /* 3. Simulate request completion with keep-alive */
    _csilk_handle_post_response(client, 1);

    /* Verify arena was returned to the pool and client->ctx.arena is NULL */
    assert(client->ctx.arena == nullptr);

    /* 4. Second request on keep-alive: on-demand allocation works again */
    csilk_arena_t* arena2 = csilk_get_arena(&client->ctx);
    assert(arena2 != nullptr);
    assert(client->ctx.arena == arena2);

    /* 5. Clean up client */
    _csilk_handle_post_response(client, 1);
    assert(client->ctx.arena == nullptr);

    pool_put(wp, client);
    free_mock_server(server);
    printf("  Lazy arena lifecycle passed!\n");
}

int
main(void)
{
    test_lazy_arena_lifecycle();
    printf("All lazy arena tests passed!\n");
    return 0;
}
