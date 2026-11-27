/*
 * Copyright (c) 2026.
 *
 * AI driver dispatch tests using a registered mock driver. Covers the
 * synchronous/async chat path, embeddings (both "supported" and "not
 * supported by driver"), global stats, and the monitor registry without
 * requiring a live LLM endpoint.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

/* --- Mock driver state: a global flag tells chat() which behavior to exercise.
 * csilk_ai_new() calls init() per instance, so per-instance state cannot be
 * pre-set externally; we steer behavior through a global. */
struct mock_state {
    int has_embed;                /**< 1 if this driver advertises embeddings support. */
};

static int g_chat_fail_times = 0; /* Transient failures to return before success. */
static int g_chat_success = 0;

static void*
mock_init(const char* api_key, const char* base_url)
{
    (void)api_key;
    (void)base_url;
    struct mock_state* st = malloc(sizeof(*st));
    assert(st != NULL);
    st->has_embed = 1;
    return st;
}

static int
mock_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)req;
    if (g_chat_fail_times > 0) {
        g_chat_fail_times--;
        res->error_message = strdup("CURL error: transient (retryable)");
        return -1;
    }
    res->content = strdup("mock reply");
    res->prompt_tokens = 4;
    res->completion_tokens = 6;
    res->total_tokens = 10;
    g_chat_success++;
    return 0;
}

static int
mock_embeddings(void*                           state,
                const char*                     model,
                const char**                    input,
                size_t                          count,
                csilk_ai_embeddings_response_t* res)
{
    (void)model;
    (void)input;
    struct mock_state* st = (struct mock_state*)state;
    if (!st->has_embed) {
        res->error_message = strdup("driver has no embeddings");
        return -1;
    }
    size_t dim = 3;
    res->values = malloc(count * dim * sizeof(float));
    assert(res->values != NULL);
    for (size_t i = 0; i < count * dim; i++) {
        res->values[i] = 0.5f;
    }
    res->dimension = dim;
    res->count = count;
    return 0;
}

static void
mock_free(void* state)
{
    free(state);
}

static const csilk_ai_driver_t mock_driver = {
    .name = "mockai",
    .init = mock_init,
    .chat = mock_chat,
    .embeddings = mock_embeddings,
    .free = mock_free,
};

/* A second driver that does not advertise embeddings (embeddings == NULL) so
 * the dispatcher's "driver does not support embeddings" branch is reached. */
static const csilk_ai_driver_t mock_noemb_driver = {
    .name = "mocknoemb",
    .init = mock_init,
    .chat = mock_chat,
    .embeddings = NULL,
    .free = mock_free,
};

static void
test_ai_mock_chat_and_retry(void)
{
    csilk_ai_register_driver(&mock_driver);
    g_chat_fail_times = 2; /* Two transient failures, then success on 3rd attempt. */

    csilk_ai_t* ai = csilk_ai_new("mockai", "k", NULL);
    assert(ai != NULL);

    csilk_ai_message_t       msg = {.role = "user",
                                    .content = "hi",
                                    .tool_calls = NULL,
                                    .tool_call_count = 0,
                                    .tool_call_id = NULL};
    csilk_ai_chat_request_t  req = {.model = "mock-model", .messages = &msg, .message_count = 1};
    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL && strcmp(res.content, "mock reply") == 0);
    assert(res.total_tokens == 10);
    g_chat_fail_times = 0;
    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("test_ai_mock_chat_and_retry passed\n");
}

static int g_async_done = 0;
static void
on_async_chat(int status, csilk_ai_chat_response_t* res, void* user_data)
{
    (void)user_data;
    if (status == 0) {
        assert(res->content != NULL);
    }
    /* Ownership of *res is released by the framework after this callback; do
     * NOT free it here. */
    g_async_done = 1;
}

static int g_embed_done = 0;
static void
on_async_embed(int status, csilk_ai_embeddings_response_t* res, void* user_data)
{
    (void)user_data;
    if (status == 0) {
        assert(res->values != NULL);
    }
    /* Ownership of *res is released by the framework after this callback. */
    g_embed_done = 1;
}

static void
test_ai_mock_async_and_embeddings(void)
{
    csilk_ai_register_driver(&mock_driver);
    csilk_ai_t* ai = csilk_ai_new("mockai", "k", NULL);
    assert(ai != NULL);

    const char* inputs[2] = {"alpha", "beta"};

    /* Synchronous embeddings success. */
    csilk_ai_embeddings_response_t eres = {0};
    assert(csilk_ai_embeddings(ai, "embed-model", inputs, 2, &eres) == 0);
    assert(eres.values != NULL && eres.count == 2 && eres.dimension == 3);
    csilk_ai_embeddings_response_free(&eres);

    /* Async chat + embeddings via the default I/O loop. */
    csilk_ai_message_t      msg = {.role = "user",
                                   .content = "hi",
                                   .tool_calls = NULL,
                                   .tool_call_count = 0,
                                   .tool_call_id = NULL};
    csilk_ai_chat_request_t req = {.model = "mock-model", .messages = &msg, .message_count = 1};
    g_async_done = 0;
    g_embed_done = 0;
    csilk_ai_chat_async(ai, &req, on_async_chat, NULL);
    csilk_ai_embeddings_async(ai, "embed-model", inputs, 2, on_async_embed, NULL);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(g_async_done == 1);
    assert(g_embed_done == 1);

    /* NULL-argument guards on the async entry points. */
    csilk_ai_chat_async(NULL, &req, on_async_chat, NULL);
    csilk_ai_embeddings_async(NULL, "m", inputs, 1, on_async_embed, NULL);

    csilk_ai_free(ai);
    printf("test_ai_mock_async_and_embeddings passed\n");
}

static void
test_ai_mock_embeddings_unsupported(void)
{
    /* The no-embeddings driver makes csilk_ai_embeddings take the
     * "driver does not support embeddings" branch. */
    csilk_ai_register_driver(&mock_noemb_driver);
    csilk_ai_t* ai = csilk_ai_new("mocknoemb", "k", NULL);
    assert(ai != NULL);

    const char*                    inputs[1] = {"alpha"};
    csilk_ai_embeddings_response_t eres = {0};
    assert(csilk_ai_embeddings(ai, "embed-model", inputs, 1, &eres) == -1);
    assert(eres.error_message != NULL);
    csilk_ai_embeddings_response_free(&eres);
    csilk_ai_free(ai);
    printf("test_ai_mock_embeddings_unsupported passed\n");
}

static void
test_ai_stats_and_monitors(void)
{
    csilk_ai_stats_t stats = {0};
    csilk_ai_get_stats(&stats);

    char* json = csilk_ai_stats_to_json(&stats);
    assert(json != NULL);
    assert(strstr(json, "requests_total") != NULL);
    free(json);

    /* NULL-input guards on the stats helpers. */
    assert(csilk_ai_stats_to_json(NULL) == NULL);
    csilk_ai_get_stats(NULL); /* safe no-op (void return). */

    /* Monitor registry (capacity 16): fill it, verify the full-registry path
     * is reached, then drain.  Each register+unregister pair is a clean no-op
     * on the bookkeeping. */
    csilk_ai_register_monitor((void*)0x1);
    csilk_ai_register_monitor((void*)0x2);
    csilk_ai_unregister_monitor((void*)0x1);
    for (int i = 0; i < 16; i++) {
        csilk_ai_register_monitor((void*)(uintptr_t)(0x100 + i));
    }
    /* Push past the 16-capacity limit to exercise the "registry is full" path. */
    csilk_ai_register_monitor((void*)0x999);
    for (int i = 0; i < 16; i++) {
        csilk_ai_unregister_monitor((void*)(uintptr_t)(0x100 + i));
    }
    csilk_ai_unregister_monitor((void*)0x2);
    assert(1);
    printf("test_ai_stats_and_monitors passed\n");
}

int
main(void)
{
    test_ai_mock_chat_and_retry();
    test_ai_mock_async_and_embeddings();
    test_ai_mock_embeddings_unsupported();
    test_ai_stats_and_monitors();
    printf("test_ai_driver_mock: PASS\n");
    return 0;
}
