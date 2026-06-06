#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/drivers/ai.h"

int
main()
{
	printf("Testing csilk_ai_chat_response_free...\n");
	{
		csilk_ai_chat_response_free(nullptr);

		csilk_ai_chat_response_t res = {0};
		res.content = strdup("hello");
		res.raw_response = strdup("{\"ok\": true}");
		res.error_message = nullptr;
		csilk_ai_chat_response_free(&res);
	}

	{
		csilk_ai_chat_response_t res = {0};
		res.error_message = strdup("error occurred");
		res.content = nullptr;
		res.raw_response = nullptr;
		csilk_ai_chat_response_free(&res);
	}

	printf("Testing csilk_ai_embeddings_response_free...\n");
	{
		csilk_ai_embeddings_response_free(nullptr);

		csilk_ai_embeddings_response_t res = {0};
		res.values = malloc(10 * sizeof(float));
		res.error_message = nullptr;
		csilk_ai_embeddings_response_free(&res);
	}

	{
		csilk_ai_embeddings_response_t res = {0};
		res.values = nullptr;
		res.error_message = strdup("error");
		csilk_ai_embeddings_response_free(&res);
	}

	printf("Testing csilk_ai_context_new/free...\n");
	{
		csilk_ai_context_t* ctx = csilk_ai_context_new(0);
		assert(ctx != nullptr);
		csilk_ai_context_add(ctx, "user", "Hello");
		assert(ctx->count == 1);
		csilk_ai_context_free(ctx);
	}

	printf("Testing csilk_ai_context sliding window...\n");
	{
		csilk_ai_context_t* ctx = csilk_ai_context_new(2);
		assert(ctx != nullptr);
		csilk_ai_context_add(ctx, "user", "msg1");
		csilk_ai_context_add(ctx, "assistant", "msg2");
		csilk_ai_context_add(ctx, "user", "msg3");
		assert(ctx->count == 2);
		assert(strcmp(ctx->messages[0].role, "assistant") == 0);
		assert(strcmp(ctx->messages[1].content, "msg3") == 0);
		csilk_ai_context_free(ctx);
	}

	printf("Testing csilk_ai_context nullptr safety...\n");
	{
		csilk_ai_context_t* ctx2 = csilk_ai_context_new(10);
		csilk_ai_context_add(nullptr, "user", "hi");
		csilk_ai_context_add(ctx2, nullptr, "hi");
		csilk_ai_context_add(ctx2, "user", nullptr);
		csilk_ai_context_clear(nullptr);
		csilk_ai_context_free(nullptr);
		csilk_ai_context_free(ctx2);
	}

	printf("Testing csilk_ai_context_clear...\n");
	{
		csilk_ai_context_t* ctx = csilk_ai_context_new(10);
		assert(ctx != nullptr);
		csilk_ai_context_add(ctx, "user", "data");
		csilk_ai_context_clear(ctx);
		assert(ctx->count == 0);
		csilk_ai_context_free(ctx);
	}

	printf("test_ai_ext: PASS\n");
	return 0;
}
