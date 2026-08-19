/**
 * @file test_zerocopy_lifecycle.c
 * @brief Comprehensive audit & regression tests for HTTP/1 zero-copy receive buffers.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"
#include "core/ctx/ctx_internal.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                           \
    } while (0)

/* ------------------------------------------------------------------ */
/* Mock Server Setup                                                  */
/* ------------------------------------------------------------------ */

static csilk_server_t*
mock_server_setup(void)
{
    csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
    s->config.max_body_size = 10 * 1024 * 1024;
    s->config.max_header_size = 64 * 1024;
    s->config.max_url_size = 8 * 1024;
    s->worker_pools = calloc(1, sizeof(worker_pool_t));
    s->worker_pools[0].server = s;
    s->worker_pool_count = 1;
    _csilk_worker_init_read_buf_pool(&s->worker_pools[0]);
    return s;
}

static void
mock_server_teardown(csilk_server_t* s)
{
    worker_pool_t* wp = &s->worker_pools[0];
    for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
        while (wp->read_buf_counts[tier] > 0) {
            free(wp->read_buf_tiers[tier][--wp->read_buf_counts[tier]]);
        }
    }
    free(s->worker_pools);
    free(s);
}

static int
test_on_message_complete(llhttp_t* p)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    /* Process URL and persistent headers without auto-dispatching server response */
    if (client->current_header_field.data && client->current_header_value.data) {
        _csilk_persist_header(
            &client->ctx, &client->current_header_field, &client->current_header_value);
        client->current_header_field.data = NULL;
        client->current_header_field.len = 0;
        client->current_header_value.data = NULL;
        client->current_header_value.len = 0;
    }

    if (client->current_url.data && client->current_url.len > 0) {
        char* url_copy = csilk_arena_strndup(
            client->ctx.arena, client->current_url.data, client->current_url.len);
        if (url_copy) {
            char* path = NULL;
            char* query = NULL;
            csilk_split_url(url_copy, &path, &query);
            client->ctx.request.path = path;
            if (query) {
                csilk_parse_query(&client->ctx, query);
                free(query);
            }
        }
        client->current_url.data = NULL;
        client->current_url.len = 0;
    }

    client->ctx.request.method = (char*)llhttp_method_name(llhttp_get_method(p));
    llhttp_pause(p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 1: Dynamic Expansion of Context Read Buffer Array             */
/* ------------------------------------------------------------------ */

static void
test_read_buffer_dynamic_expansion(void)
{
    csilk_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.arena = csilk_arena_new(4096);
    c.read_buffers = c.read_buffers_embedded;
    c.read_buffers_capacity = 16;
    c.read_buf_sizes = c.read_buf_sizes_embedded;

    /* Register 40 buffers (exceeding 16 embedded capacity) */
    char* bufs[40];
    for (int i = 0; i < 40; i++) {
        bufs[i] = malloc(128);
        int r = _csilk_ctx_register_pooled_read_buffer(&c, bufs[i], 0);
        if (r != 0) {
            FAIL("Failed to dynamically register read buffer");
            csilk_arena_free(c.arena);
            return;
        }
    }

    if (c.read_buffers_count != 40 || c.read_buffers_capacity < 40) {
        FAIL("Buffer capacity did not expand as expected");
        csilk_arena_free(c.arena);
        return;
    }

    /* Verify all registered pointers are intact */
    for (int i = 0; i < 40; i++) {
        if (c.read_buffers[i] != bufs[i]) {
            FAIL("Buffer pointer corrupted after expansion");
            csilk_arena_free(c.arena);
            return;
        }
    }

    /* Cleanup */
    csilk_ctx_cleanup(&c);

    if (c.read_buffers_count != 0 || c.read_buffers != c.read_buffers_embedded) {
        FAIL("Cleanup did not restore embedded buffer state");
        csilk_arena_free(c.arena);
        return;
    }

    csilk_arena_free(c.arena);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 2: Single-Chunk Body Zero-Copy Borrowed View Lifecycle        */
/* ------------------------------------------------------------------ */

static void
test_single_chunk_body_borrowed_view(void)
{
    csilk_server_t* s = mock_server_setup();
    worker_pool_t*  wp = &s->worker_pools[0];

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = s;
    client.owner_pool = wp;
    client.ctx.server = s;
    client.ctx.arena = csilk_arena_new(4096);
    client.ctx.read_buffers = client.ctx.read_buffers_embedded;
    client.ctx.read_buffers_capacity = 16;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    /* Allocate buffer from pool tier 0 (4KB) */
    char*       read_buf = (char*)wp->read_buf_tiers[0][--wp->read_buf_counts[0]];
    const char* req_text = "POST /api/data HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Content-Length: 13\r\n"
                           "\r\n"
                           "Hello, World!";
    size_t      req_len = strlen(req_text);
    memcpy(read_buf, req_text, req_len);

    /* Register buffer */
    _csilk_ctx_register_pooled_read_buffer(&client.ctx, read_buf, CSILK_READ_BUF_4KB);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = test_on_message_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;

    llhttp_execute(&client.parser, read_buf, req_len);

    /* Assertions */
    if (!client.ctx.request.body || client.ctx.request.body_len != 13 ||
        memcmp(client.ctx.request.body, "Hello, World!", 13) != 0) {
        FAIL("Request body content mismatch");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }
    if (client.ctx.request.body_ownership != CSILK_OWN_BORROWED) {
        FAIL("Expected CSILK_OWN_BORROWED for single-chunk body");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }
    /* Verify body pointer directly points into read_buf */
    if (client.ctx.request.body < read_buf ||
        client.ctx.request.body >= read_buf + CSILK_READ_BUF_4KB) {
        FAIL("Borrowed body does not point into read buffer");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* Cleanup context -> returns read_buf back to pool tier 0 */
    csilk_ctx_cleanup(&client.ctx);

    if (wp->read_buf_counts[0] != CSILK_READ_BUF_POOL_SIZE) {
        FAIL("Read buffer was not returned to pool on cleanup");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 3: Multi-Chunk Body Split and Upgrade to Managed Heap Buffer  */
/* ------------------------------------------------------------------ */

static void
test_multi_chunk_body_upgrade(void)
{
    csilk_server_t* s = mock_server_setup();
    worker_pool_t*  wp = &s->worker_pools[0];

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = s;
    client.owner_pool = wp;
    client.ctx.server = s;
    client.ctx.arena = csilk_arena_new(4096);
    client.ctx.read_buffers = client.ctx.read_buffers_embedded;
    client.ctx.read_buffers_capacity = 16;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = test_on_message_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;

    /* Chunk 1: Headers + First part of body */
    char*       buf1 = (char*)wp->read_buf_tiers[0][--wp->read_buf_counts[0]];
    const char* part1 = "POST /upload HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 10\r\n"
                        "\r\n"
                        "ABCDE";
    memcpy(buf1, part1, strlen(part1));
    _csilk_ctx_register_pooled_read_buffer(&client.ctx, buf1, CSILK_READ_BUF_4KB);
    llhttp_execute(&client.parser, buf1, strlen(part1));

    if (client.ctx.request.body_ownership != CSILK_OWN_BORROWED) {
        FAIL("Chunk 1 body should be borrowed");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* Chunk 2: Second part of body in a DIFFERENT buffer */
    char*       buf2 = (char*)wp->read_buf_tiers[0][--wp->read_buf_counts[0]];
    const char* part2 = "FGHIJ";
    memcpy(buf2, part2, strlen(part2));
    _csilk_ctx_register_pooled_read_buffer(&client.ctx, buf2, CSILK_READ_BUF_4KB);
    llhttp_execute(&client.parser, buf2, strlen(part2));

    /* Verify upgrade to managed heap */
    if (client.ctx.request.body_ownership != CSILK_OWN_HEAP ||
        client.ctx.request.body_is_managed != 1) {
        FAIL("Split body should be upgraded to CSILK_OWN_HEAP");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    if (strcmp(client.ctx.request.body, "ABCDEFGHIJ") != 0) {
        FAIL("Assembled split body mismatch");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* Cleanup -> frees managed body and returns both buf1 & buf2 to pool */
    csilk_ctx_cleanup(&client.ctx);

    if (wp->read_buf_counts[0] != CSILK_READ_BUF_POOL_SIZE) {
        FAIL("All buffers must be returned to pool");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 4: Async Response Lifecycle & Zero-Copy Preservation          */
/* ------------------------------------------------------------------ */

static void
test_async_response_buffer_safety(void)
{
    csilk_server_t* s = mock_server_setup();
    worker_pool_t*  wp = &s->worker_pools[0];

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = s;
    client.owner_pool = wp;
    client.ctx.server = s;
    client.ctx.arena = csilk_arena_new(4096);
    client.ctx.read_buffers = client.ctx.read_buffers_embedded;
    client.ctx.read_buffers_capacity = 16;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    char*       read_buf = (char*)wp->read_buf_tiers[0][--wp->read_buf_counts[0]];
    const char* req = "POST /async HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "ASYNC";
    memcpy(read_buf, req, strlen(req));
    _csilk_ctx_register_pooled_read_buffer(&client.ctx, read_buf, CSILK_READ_BUF_4KB);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_body = on_body;
    settings.on_message_complete = test_on_message_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;
    llhttp_execute(&client.parser, read_buf, strlen(req));

    /* Acquire async lease */
    csilk_async_token_t token = csilk_ctx_acquire_async(&client.ctx);
    if (csilk_async_token_validate(&token) != 1) {
        FAIL("Async token should be valid");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* In async phase: read buffer is NOT recycled yet */
    if (wp->read_buf_counts[0] != CSILK_READ_BUF_POOL_SIZE - 1) {
        FAIL("Read buffer must remain leased during async processing");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* Verify borrowed body is accessible and valid during async */
    if (memcmp(client.ctx.request.body, "ASYNC", 5) != 0) {
        FAIL("Async body corrupted");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* Async task finishes -> releases token and cleans up context */
    csilk_ctx_release_async(&token);
    csilk_ctx_cleanup(&client.ctx);

    if (wp->read_buf_counts[0] != CSILK_READ_BUF_POOL_SIZE) {
        FAIL("Read buffer must be returned to pool after async completion");
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 5: HTTP Keep-Alive Cross-Request Isolation                    */
/* ------------------------------------------------------------------ */

static void
test_keepalive_multi_cycle_isolation(void)
{
    csilk_server_t* s = mock_server_setup();
    worker_pool_t*  wp = &s->worker_pools[0];

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = s;
    client.owner_pool = wp;
    client.ctx.server = s;
    client.ctx.arena = csilk_arena_new(4096);
    client.ctx.read_buffers = client.ctx.read_buffers_embedded;
    client.ctx.read_buffers_capacity = 16;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = test_on_message_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;

    for (int cycle = 0; cycle < 50; cycle++) {
        char* buf = (char*)wp->read_buf_tiers[0][--wp->read_buf_counts[0]];
        char  req[256];
        snprintf(req,
                 sizeof(req),
                 "POST /cycle/%d HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "X-Cycle: %d\r\n"
                 "Content-Length: 8\r\n"
                 "\r\n"
                 "DATA_%03d",
                 cycle,
                 cycle,
                 cycle);

        size_t len = strlen(req);
        memcpy(buf, req, len);
        _csilk_ctx_register_pooled_read_buffer(&client.ctx, buf, CSILK_READ_BUF_4KB);

        llhttp_execute(&client.parser, buf, len);

        /* Verify current cycle values */
        char expected_body[16];
        snprintf(expected_body, sizeof(expected_body), "DATA_%03d", cycle);
        if (!client.ctx.request.body || client.ctx.request.body_len != 8 ||
            memcmp(client.ctx.request.body, expected_body, 8) != 0) {
            FAIL("Keep-alive body mismatch across cycles");
            csilk_ctx_cleanup(&client.ctx);
            csilk_arena_free(client.ctx.arena);
            mock_server_teardown(s);
            return;
        }

        /* Post-response cleanup and resume for next keep-alive cycle */
        csilk_ctx_cleanup(&client.ctx);
        llhttp_resume(&client.parser);

        if (wp->read_buf_counts[0] != CSILK_READ_BUF_POOL_SIZE) {
            FAIL("Buffer leak in keep-alive cycle");
            csilk_arena_free(client.ctx.arena);
            mock_server_teardown(s);
            return;
        }
    }

    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 6: Chunk Boundary Random Fuzzing Test                         */
/* ------------------------------------------------------------------ */

static void
test_chunk_boundary_random_fuzzing(void)
{
    csilk_server_t* s = mock_server_setup();
    worker_pool_t*  wp = &s->worker_pools[0];

    const char* full_request = "POST /api/v1/resource?query=123&filter=active HTTP/1.1\r\n"
                               "Host: api.csilk.io\r\n"
                               "User-Agent: Csilk-Fuzzer/2.0\r\n"
                               "Content-Type: application/json\r\n"
                               "X-Custom-Header-1: AlphaBetaGamma\r\n"
                               "X-Custom-Header-2: 9876543210\r\n"
                               "Content-Length: 45\r\n"
                               "\r\n"
                               "{\"status\":\"ok\",\"payload\":[1,2,3,4,5,6,7,8,9]}";
    size_t      full_len = strlen(full_request);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = test_on_message_complete;
    s->settings = settings;

    srand(1337); /* Deterministic seed for reproducible fuzzing */

    const int NUM_ITERATIONS = 1000;
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        csilk_client_t client;
        memset(&client, 0, sizeof(client));
        client.server = s;
        client.owner_pool = wp;
        client.ctx.server = s;
        client.ctx.arena = csilk_arena_new(8192);
        client.ctx.read_buffers = client.ctx.read_buffers_embedded;
        client.ctx.read_buffers_capacity = 16;
        client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
        _csilk_set_internal_client(&client.ctx, &client);

        llhttp_init(&client.parser, HTTP_REQUEST, &settings);
        client.parser.data = &client;

        size_t offset = 0;
        while (offset < full_len) {
            /* Random chunk size from 1 to 17 bytes */
            size_t chunk_size = 1 + (size_t)(rand() % 17);
            if (offset + chunk_size > full_len) {
                chunk_size = full_len - offset;
            }

            char* chunk_buf = malloc(chunk_size + 1);
            memcpy(chunk_buf, full_request + offset, chunk_size);
            chunk_buf[chunk_size] = '\0';

            _csilk_ctx_register_pooled_read_buffer(&client.ctx, chunk_buf, 0);

            enum llhttp_errno err = llhttp_execute(&client.parser, chunk_buf, chunk_size);
            if (err != HPE_OK && err != HPE_PAUSED) {
                printf("Fuzzing parse error at offset %zu / %zu (chunk_size %zu): %s (%s)\n",
                       offset,
                       full_len,
                       chunk_size,
                       llhttp_errno_name(err),
                       client.parser.reason ? client.parser.reason : "none");
                printf("Chunk content: '%.*s'\n", (int)chunk_size, chunk_buf);
                FAIL("Parse error during chunk boundary fuzzing");
                csilk_ctx_cleanup(&client.ctx);
                csilk_arena_free(client.ctx.arena);
                mock_server_teardown(s);
                return;
            }

            offset += chunk_size;
        }

        /* Verify parsed request body */
        if (!client.ctx.request.body || client.ctx.request.body_len != 45 ||
            memcmp(client.ctx.request.body,
                   "{\"status\":\"ok\",\"payload\":[1,2,3,4,5,6,7,8,9]}",
                   45) != 0) {
            FAIL("Fuzzed request body mismatch");
            csilk_ctx_cleanup(&client.ctx);
            csilk_arena_free(client.ctx.arena);
            mock_server_teardown(s);
            return;
        }

        /* Verify headers persisted to arena */
        const char* h1 = csilk_get_header(&client.ctx, "X-Custom-Header-1");
        if (!h1 || strcmp(h1, "AlphaBetaGamma") != 0) {
            FAIL("Fuzzed header 1 mismatch");
            csilk_ctx_cleanup(&client.ctx);
            csilk_arena_free(client.ctx.arena);
            mock_server_teardown(s);
            return;
        }

        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
    }

    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Main Test Runner                                                   */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Zero-Copy Receive Buffer Lifecycle Tests ===\n\n");

    printf("--- Buffer Registration & Array Growth ---\n");
    test_read_buffer_dynamic_expansion();

    printf("\n--- Single-Chunk & Multi-Chunk Body Views ---\n");
    test_single_chunk_body_borrowed_view();
    test_multi_chunk_body_upgrade();

    printf("\n--- Async Lifecycle & Keep-Alive Isolation ---\n");
    test_async_response_buffer_safety();
    test_keepalive_multi_cycle_isolation();

    printf("\n--- Chunk Boundary Random Fuzzing (1,000 iterations) ---\n");
    test_chunk_boundary_random_fuzzing();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
