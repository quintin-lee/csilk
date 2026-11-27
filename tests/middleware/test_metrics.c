/**
 * @file test_metrics.c
 * @brief Test for Prometheus metrics middleware.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "../../src/core/internal/obs_registry.h"
#ifdef CSILK_HAS_DB
#include "csilk/drivers/db.h"
#endif
#ifdef CSILK_HAS_AI
#include "csilk/drivers/ai.h"
#endif

static int g_custom_collector_calls = 0;

static void
dummy_handler(csilk_ctx_t* c)
{
    (void)c;
}

static void
custom_test_collector(csilk_ctx_t* c, csilk_metrics_emit_fn emit, void* writer)
{
    (void)c;
    g_custom_collector_calls++;
    emit(writer, "csilk_test_custom_metric %d\n", g_custom_collector_calls);
}

static void
noop_test_collector(csilk_ctx_t* c, csilk_metrics_emit_fn emit, void* writer)
{
    (void)c;
    (void)emit;
    (void)writer;
}

#ifdef CSILK_HAS_AI
static void*
metrics_mock_ai_init(const char* api_key, const char* base_url)
{
    (void)api_key;
    (void)base_url;
    return (void*)0x1;
}

static int
metrics_mock_ai_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    (void)req;
    res->content = strdup("ok");
    res->prompt_tokens = 3;
    res->completion_tokens = 4;
    res->total_tokens = 7;
    return 0;
}

static void
metrics_mock_ai_free(void* state)
{
    (void)state;
}
#endif

void
test_metrics()
{
    printf("Testing Metrics middleware...\n");

    csilk_handler_t handlers[] = {dummy_handler, nullptr};
    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_test_ctx_set_handlers(c, handlers);

    /* Simulate a request through middleware */
    csilk_metrics_middleware(c, NULL);

    /* Call metrics handler to get output */
    csilk_metrics_handler(c);

    /* Verify output */
    assert(csilk_get_status(c) == CSILK_STATUS_OK);
    const char* body = csilk_get_response_body(c, nullptr);
    assert(body != nullptr);
    assert(strstr(body, "http_requests_total_agg 1") != nullptr);
    assert(strstr(body, "http_request_duration_microseconds_agg") != nullptr);
    assert(strstr(body, "process_resident_memory_bytes") != nullptr);
    assert(strstr(body, "csilk_server_active_connections") != nullptr);
    assert(strstr(body, "csilk_security_rate_limit_blocks_total") != nullptr);
    assert(strstr(body, "csilk_db_operations_total") == nullptr);
    assert(strstr(body, "csilk_ai_requests_total") == nullptr);
    assert(strstr(body, "csilk_mq_messages_total") == nullptr);

    csilk_test_ctx_free(c);

    /* Second request: run the metrics middleware so it records the request. */
    c = csilk_test_ctx_new();
    csilk_test_ctx_set_handlers(c, handlers);
    csilk_metrics_middleware(c, NULL);
    csilk_metrics_handler(c);
    body = csilk_get_response_body(c, nullptr);
    assert(strstr(body, "http_requests_total_agg 2") != nullptr);

    csilk_test_ctx_free(c);
    printf("Metrics test passed!\n");
}

static void
test_metrics_optional_collectors(void)
{
    printf("Testing Metrics optional collector registry...\n");

#ifdef CSILK_HAS_DB
    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != nullptr);
    assert(csilk_db_exec(pool, "CREATE TABLE metrics_t (id INTEGER);") == 0);
    csilk_db_pool_free(pool);
#endif

#ifdef CSILK_HAS_AI
    static const csilk_ai_driver_t mock_driver = {
        .name = "metrics_mock",
        .init = metrics_mock_ai_init,
        .chat = metrics_mock_ai_chat,
        .embeddings = nullptr,
        .free = metrics_mock_ai_free,
    };
    csilk_ai_register_driver(&mock_driver);
    csilk_ai_t* ai = csilk_ai_new("metrics_mock", "k", nullptr);
    assert(ai != nullptr);
    csilk_ai_chat_request_t  req = {.model = "m", .user = "hi"};
    csilk_ai_chat_response_t res = {0};
    assert(csilk_ai_chat(ai, &req, &res) == 0);
    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
#endif

    /* Invalid collector registrations must fail cleanly */
    assert(_csilk_metrics_register_collector(nullptr, custom_test_collector) == -1);
    assert(_csilk_metrics_register_collector("", custom_test_collector) == -1);
    assert(_csilk_metrics_register_collector("custom", nullptr) == -1);

    /* Idempotent registration by name */
    assert(_csilk_metrics_register_collector("test_custom", noop_test_collector) == 0);
    assert(_csilk_metrics_register_collector("test_custom", custom_test_collector) == 0);

    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_metrics_handler(c);
    assert(csilk_get_status(c) == CSILK_STATUS_OK);
    const char* body = csilk_get_response_body(c, nullptr);
    assert(body != nullptr);
    assert(strstr(body, "csilk_test_custom_metric 1") != nullptr);
#ifdef CSILK_HAS_DB
    assert(strstr(body, "csilk_db_operations_total{type=\"exec\"} 1") != nullptr);
#endif
#ifdef CSILK_HAS_AI
    assert(strstr(body, "csilk_ai_requests_total 1") != nullptr);
#endif
    csilk_test_ctx_free(c);

    /* Fill remaining slots up to CSILK_OBS_MAX_COLLECTORS and verify cap */
    static const char* const extra_names[CSILK_OBS_MAX_COLLECTORS] = {
        "slot_a", "slot_b", "slot_c", "slot_d", "slot_e", "slot_f", "slot_g", "slot_h"};
    int filled = 0;
    for (int i = 0; i < CSILK_OBS_MAX_COLLECTORS; i++) {
        if (_csilk_metrics_register_collector(extra_names[i], noop_test_collector) == 0) {
            filled++;
        }
    }
    assert(filled >= 1);
    assert(_csilk_metrics_register_collector("overflow_slot", noop_test_collector) == -1);

    printf("Metrics collector registry test passed!\n");
}

int
main()
{
    test_metrics();
    test_metrics_optional_collectors();
    return 0;
}
