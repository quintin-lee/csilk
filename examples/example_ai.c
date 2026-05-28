/**
 * @file example_ai.c
 * @brief Example usage of the csilk AI unified interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

static void
on_stream_chunk(const char* chunk, void* user_data)
{
	(void)user_data;
	printf("%s", chunk);
	fflush(stdout);
}

static void
on_async_chat(int status, csilk_ai_chat_response_t* res, void* user_data)
{
	(void)user_data;
	if (status == 0) {
		printf("\n[Async Response Received]\n%s\n", res->content);
	} else {
		printf("\n[Async chat call failed: %s]\n",
		       res->error_message ? res->error_message : "Unknown error");
	}
	csilk_ai_chat_response_free(res);
	/* In a real app, you might signal completion or cleanup here */
}

int
main()
{
	const char* api_key = getenv("AGENT_API_KEY");
	const char* api_base = getenv("AGENT_API_BASE");
	const char* model_name = getenv("AGENT_MODEL_NAME");

	if (!api_key) {
		printf("Please set AGENT_API_KEY environment variable to run "
		       "this example.\n");
		return 0;
	}

	if (!model_name) {
		model_name = "gpt-3.5-turbo";
	}

	/* Create an OpenAI-compatible AI instance */
	printf("Initializing AI driver...\n");
	printf("  Base URL: %s\n", api_base ? api_base : "https://api.openai.com/v1");
	printf("  Model:    %s\n\n", model_name);

	csilk_ai_t* ai = csilk_ai_new("openai", api_key, api_base);
	if (!ai) {
		fprintf(stderr, "Failed to initialize AI driver\n");
		return 1;
	}

	/* Prepare a chat request */
	csilk_ai_message_t messages[] = {{"system", "You are a helpful assistant."},
					 {"user", "介绍一下自己，并用一句话总结 C 语言的优势。"}};

	/* 1. Non-streaming call with enriched parameters */
	csilk_ai_chat_request_t req = {
	    .model = model_name,
	    .messages = messages,
	    .message_count = 2,
	    .temperature = 0.7,
	    .top_p = 0.9,
	    .max_tokens = 512,
	    .user = "csilk-test-user",
	    .timeout_ms = 30000 /* 30s timeout */
	};

	csilk_ai_chat_response_t res;
	printf("--- [1] Non-streaming call ---\n");
	if (csilk_ai_chat(ai, &req, &res) == 0) {
		printf("AI Response:\n%s\n", res.content);
		printf("\nUsage: prompt=%d, completion=%d, total=%d\n\n",
		       res.prompt_tokens,
		       res.completion_tokens,
		       res.total_tokens);
		csilk_ai_chat_response_free(&res);
	} else {
		fprintf(stderr,
			"Non-streaming chat call failed: %s\n",
			res.error_message ? res.error_message : "Unknown error");
		csilk_ai_chat_response_free(&res);
	}

	/* 2. Streaming call */
	req.stream = true;
	req.on_chunk = on_stream_chunk;
	printf("--- [2] Streaming call ---\n");
	if (csilk_ai_chat(ai, &req, &res) == 0) {
		printf("\n\n(Full content captured: %zu bytes)\n", strlen(res.content));
		csilk_ai_chat_response_free(&res);
	} else {
		fprintf(stderr,
			"Streaming chat call failed: %s\n",
			res.error_message ? res.error_message : "Unknown error");
		csilk_ai_chat_response_free(&res);
	}

	/* 3. Asynchronous call */
	printf("\n--- [3] Asynchronous call (Non-blocking) ---\n");
	req.stream = false;
	req.on_chunk = NULL;
	csilk_ai_chat_async(ai, &req, on_async_chat, NULL);
	printf("Async request dispatched, main thread continues...\n");

	/* Simulate doing other work */
	for (int i = 0; i < 5; i++) {
		printf("Doing other work... %d\n", i);
		uv_run(uv_default_loop(), UV_RUN_ONCE);
	}
	/* Ensure async callback is triggered */
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	/* 4. Embeddings */
	printf("\n--- [4] Embeddings ---\n");
	const char* emb_input[] = {"Hello world", "Artificial Intelligence is great"};
	csilk_ai_embeddings_response_t eres;
	if (csilk_ai_embeddings(ai, "text-embedding-3-small", emb_input, 2, &eres) == 0) {
		printf(
		    "Embeddings generated: count=%zu, dimension=%zu\n", eres.count, eres.dimension);
		if (eres.count > 0 && eres.dimension > 0) {
			printf("First 5 values of first vector: [%.4f, %.4f, "
			       "%.4f, %.4f, %.4f]\n",
			       eres.values[0],
			       eres.values[1],
			       eres.values[2],
			       eres.values[3],
			       eres.values[4]);
		}
		csilk_ai_embeddings_response_free(&eres);
	} else {
		fprintf(stderr,
			"Embeddings call failed: %s\n",
			eres.error_message ? eres.error_message : "Unknown error");
		csilk_ai_embeddings_response_free(&eres);
	}

	/* 5. Tool Calling (Function Calling) */
	printf("\n--- [5] Tool Calling ---\n");
	csilk_ai_message_t tool_messages[] = {{"user", "What's the weather like in Shanghai?"}};

	/* Define a tool using cJSON */
	cJSON* params = cJSON_CreateObject();
	cJSON_AddStringToObject(params, "type", "object");
	cJSON* props = cJSON_CreateObject();
	cJSON* loc = cJSON_CreateObject();
	cJSON_AddStringToObject(loc, "type", "string");
	cJSON_AddStringToObject(loc, "description", "The city and state, e.g. San Francisco, CA");
	cJSON_AddItemToObject(props, "location", loc);
	cJSON_AddItemToObject(params, "properties", props);
	cJSON* req_arr = cJSON_CreateArray();
	cJSON_AddItemToArray(req_arr, cJSON_CreateString("location"));
	cJSON_AddItemToObject(params, "required", req_arr);

	csilk_ai_tool_t tools[1] = {
	    {.type = "function",
	     .function = {.name = "get_current_weather",
			  .description = "Get the current weather in a given location",
			  .parameters_json = params}}};

	csilk_ai_chat_request_t treq = {.model = model_name,
					.messages = tool_messages,
					.message_count = 1,
					.tools = tools,
					.tool_count = 1,
					.tool_choice = "auto"};

	if (csilk_ai_chat(ai, &treq, &res) == 0) {
		if (res.tool_call_count > 0) {
			printf("Model requested tool calls:\n");
			for (size_t i = 0; i < res.tool_call_count; i++) {
				printf("  - ID: %s\n", res.tool_calls[i].id);
				printf("    Name: %s\n", res.tool_calls[i].name);
				printf("    Args: %s\n", res.tool_calls[i].arguments);
			}
		} else {
			printf("AI Response: %s\n", res.content);
		}
		csilk_ai_chat_response_free(&res);
	} else {
		fprintf(stderr,
			"Tool calling failed: %s\n",
			res.error_message ? res.error_message : "Unknown error");
		csilk_ai_chat_response_free(&res);
	}
	cJSON_Delete(params);

	csilk_ai_free(ai);
	return 0;
}
