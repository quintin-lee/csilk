/**
 * @file test_headers_fragmented.c
 * @brief Targeted tests for fragmented llhttp header callbacks and zero-copy view merging.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* Test 1: User-Specified Fuzz Case: Content-T + ype + : app/ + json  */
/* ------------------------------------------------------------------ */

static void
test_fuzz_case_content_type_split(void)
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
    client.ctx.read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin = on_message_begin;
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_field_complete = on_header_field_complete;
    settings.on_header_value = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete = on_headers_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;

    /* Feed prefix */
    const char* pfx = "GET / HTTP/1.1\r\n";
    llhttp_execute(&client.parser, pfx, strlen(pfx));

    /* Chunk 1: "Content-T" */
    const char* c1 = "Content-T";
    llhttp_execute(&client.parser, c1, strlen(c1));

    /* Chunk 2: "ype" */
    const char* c2 = "ype";
    llhttp_execute(&client.parser, c2, strlen(c2));

    /* Chunk 3: ": application/" */
    const char* c3 = ": application/";
    llhttp_execute(&client.parser, c3, strlen(c3));

    /* Chunk 4: "json" */
    const char* c4 = "json\r\n\r\n";
    llhttp_execute(&client.parser, c4, strlen(c4));

    /* Verify Content-Type in header map */
    const char* ct = csilk_get_header(&client.ctx, "Content-Type");
    if (!ct) {
        FAIL("Content-Type header not found in header_map");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    if (strcmp(ct, "application/json") != 0) {
        printf("  Expected 'application/json', got '%s'\n", ct);
        FAIL("Content-Type header value mismatch");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    /* Case-insensitive lookup check */
    const char* ct_lower = csilk_get_header(&client.ctx, "content-type");
    if (!ct_lower || strcmp(ct_lower, "application/json") != 0) {
        FAIL("Case-insensitive lookup for content-type failed");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    csilk_ctx_cleanup(&client.ctx);
    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 2: Contiguous Zero-Copy Pointer Extension Verification        */
/* ------------------------------------------------------------------ */

static void
test_contiguous_chunk_zero_copy(void)
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
    client.ctx.read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin = on_message_begin;
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_field_complete = on_header_field_complete;
    settings.on_header_value = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete = on_headers_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;

    char full_msg[] = "POST /api HTTP/1.1\r\n"
                      "X-Long-Header-Name: Multi-Part-Long-Value-String\r\n"
                      "\r\n";

    /* Feed in contiguous slices from the exact same contiguous buffer */
    /* Slice 1: POST /api HTTP/1.1\r\nX-Long- */
    llhttp_execute(&client.parser, full_msg, 27);
    /* Slice 2: Header-Name: Multi- */
    llhttp_execute(&client.parser, full_msg + 27, 19);
    /* Slice 3: Part-Long-Value-String\r\n\r\n */
    llhttp_execute(&client.parser, full_msg + 46, strlen(full_msg + 46));

    const char* val = csilk_get_header(&client.ctx, "X-Long-Header-Name");
    if (!val || strcmp(val, "Multi-Part-Long-Value-String") != 0) {
        FAIL("Contiguous header value mismatch");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    csilk_ctx_cleanup(&client.ctx);
    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 3: Non-Contiguous Chunks (Separate Buffers) & Empty Header    */
/* ------------------------------------------------------------------ */

static void
test_non_contiguous_and_empty_headers(void)
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
    client.ctx.read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
    client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
    _csilk_set_internal_client(&client.ctx, &client);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin = on_message_begin;
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_field_complete = on_header_field_complete;
    settings.on_header_value = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete = on_headers_complete;
    s->settings = settings;

    llhttp_init(&client.parser, HTTP_REQUEST, &settings);
    client.parser.data = &client;

    /* Feed non-contiguous buffers */
    const char* chunks[] = {"GET /test HTTP/1.1\r\n",
                            "X-Empty-Header:\r\n",
                            "X-Frag-",
                            "Key: Frag-",
                            "Value-12345\r\n",
                            "Authorization: Bearer ",
                            "token_abcdef_123456\r\n",
                            "\r\n"};

    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        char* buf = strdup(chunks[i]);
        _csilk_ctx_register_pooled_read_buffer(&client.ctx, buf, 0);
        llhttp_execute(&client.parser, buf, strlen(buf));
    }

    const char* empty_h = csilk_get_header(&client.ctx, "X-Empty-Header");
    if (!empty_h || strcmp(empty_h, "") != 0) {
        FAIL("Empty header value mismatch");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    const char* frag_h = csilk_get_header(&client.ctx, "X-Frag-Key");
    if (!frag_h || strcmp(frag_h, "Frag-Value-12345") != 0) {
        FAIL("Fragmented header value mismatch");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    const char* auth_h = csilk_get_header(&client.ctx, "Authorization");
    if (!auth_h || strcmp(auth_h, "Bearer token_abcdef_123456") != 0) {
        FAIL("Auth header mismatch");
        csilk_ctx_cleanup(&client.ctx);
        csilk_arena_free(client.ctx.arena);
        mock_server_teardown(s);
        return;
    }

    csilk_ctx_cleanup(&client.ctx);
    csilk_arena_free(client.ctx.arena);
    mock_server_teardown(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 4: Header Fragment Fuzzing across 10,000 iterations           */
/* ------------------------------------------------------------------ */

static void
test_header_fragment_fuzzing(void)
{
    csilk_server_t* s = mock_server_setup();
    worker_pool_t*  wp = &s->worker_pools[0];

    const char* req = "POST /api/v1/fuzz HTTP/1.1\r\n"
                      "Host: example.org\r\n"
                      "Content-Type: application/json; charset=utf-8\r\n"
                      "X-Custom-Field-Alpha: 112233445566778899\r\n"
                      "X-Custom-Field-Beta: Some-Long-String-Value-With-Special-Characters\r\n"
                      "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                      "\r\n";
    size_t      req_len = strlen(req);

    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin = on_message_begin;
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_field_complete = on_header_field_complete;
    settings.on_header_value = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete = on_headers_complete;
    s->settings = settings;

    srand(42);

    for (int iter = 0; iter < 10000; iter++) {
        csilk_client_t client;
        memset(&client, 0, sizeof(client));
        client.server = s;
        client.owner_pool = wp;
        client.ctx.server = s;
        client.ctx.arena = csilk_arena_new(4096);
        client.ctx.read_buffers = client.ctx.read_buffers_embedded;
        client.ctx.read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
        client.ctx.read_buf_sizes = client.ctx.read_buf_sizes_embedded;
        _csilk_set_internal_client(&client.ctx, &client);

        llhttp_init(&client.parser, HTTP_REQUEST, &settings);
        client.parser.data = &client;

        size_t offset = 0;
        while (offset < req_len) {
            size_t chunk = 1 + (rand() % 9);
            if (offset + chunk > req_len) {
                chunk = req_len - offset;
            }

            char* buf = malloc(chunk + 1);
            memcpy(buf, req + offset, chunk);
            buf[chunk] = '\0';
            _csilk_ctx_register_pooled_read_buffer(&client.ctx, buf, 0);

            enum llhttp_errno err = llhttp_execute(&client.parser, buf, chunk);

            if (err != HPE_OK) {
                FAIL("Parse error in header fuzzer");
                csilk_ctx_cleanup(&client.ctx);
                csilk_arena_free(client.ctx.arena);
                mock_server_teardown(s);
                return;
            }

            offset += chunk;
        }

        const char* ct = csilk_get_header(&client.ctx, "Content-Type");
        if (!ct || strcmp(ct, "application/json; charset=utf-8") != 0) {
            FAIL("Fuzzed Content-Type mismatch");
            csilk_ctx_cleanup(&client.ctx);
            csilk_arena_free(client.ctx.arena);
            mock_server_teardown(s);
            return;
        }

        const char* beta = csilk_get_header(&client.ctx, "X-Custom-Field-Beta");
        if (!beta || strcmp(beta, "Some-Long-String-Value-With-Special-Characters") != 0) {
            FAIL("Fuzzed Beta header mismatch");
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
/* Main Runner                                                        */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Fragmented Header Callbacks & Zero-Copy View Tests ===\n\n");

    printf("--- User Fuzz Case: Content-Type Split ---\n");
    test_fuzz_case_content_type_split();

    printf("\n--- Contiguous Zero-Copy Pointer Extension ---\n");
    test_contiguous_chunk_zero_copy();

    printf("\n--- Non-Contiguous & Empty Headers ---\n");
    test_non_contiguous_and_empty_headers();

    printf("\n--- Header Fragment Fuzzing (10,000 Iterations) ---\n");
    test_header_fragment_fuzzing();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
