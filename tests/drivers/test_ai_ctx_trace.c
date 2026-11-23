#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"
#include "csilk/middleware/otlp_trace.h"
#include "csilk/test/test.h"

static void*
mock_init(const char* api_key, const char* base_url)
{
    (void)api_key;
    (void)base_url;
    return (void*)0x1;
}

static int
mock_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    (void)req;
    res->content = strdup("mock reply from ctx");
    res->prompt_tokens = 10;
    res->completion_tokens = 20;
    res->total_tokens = 30;
    return 0;
}

static void
mock_free(void* state)
{
    (void)state;
}

static const csilk_ai_driver_t s_ctx_mock_driver = {
    .name = "ctx_mock",
    .init = mock_init,
    .chat = mock_chat,
    .embeddings = NULL,
    .free = mock_free,
};

int
main(void)
{
    csilk_ai_register_driver(&s_ctx_mock_driver);

    csilk_ai_t* ai = csilk_ai_new("ctx_mock", "key", "http://localhost");
    assert(ai != NULL);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);

    csilk_ai_chat_request_t req = {
        .model = "mock-gpt",
        .user = "test",
    };
    csilk_ai_chat_response_t res = {0};

    /* 1. Context-aware call with ctx */
    int rc = csilk_ai_chat_ctx(ai, ctx, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL);
    assert(strcmp(res.content, "mock reply from ctx") == 0);

    /* Verify trace was attached to ctx */
    const csilk_ai_trace_t* trace = csilk_otlp_ctx_get_ai_trace(ctx);
    assert(trace != NULL);

    csilk_ai_chat_response_free(&res);

    /* 2. Null context call */
    memset(&res, 0, sizeof(res));
    rc = csilk_ai_chat_ctx(ai, NULL, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL);
    csilk_ai_chat_response_free(&res);

    /* 3. Null argument checks */
    assert(csilk_ai_chat_ctx(NULL, ctx, &req, &res) == -1);
    assert(csilk_ai_chat_ctx(ai, ctx, NULL, &res) == -1);
    assert(csilk_ai_chat_ctx(ai, ctx, &req, NULL) == -1);

    csilk_ai_free(ai);
    csilk_test_ctx_free(ctx);

    printf("test_ai_ctx_trace passed successfully!\n");
    return 0;
}
