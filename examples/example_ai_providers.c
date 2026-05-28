#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

/*
 * Custom Echo Driver — demonstrates how to register a third-party AI
 * provider without modifying the framework source.
 */
typedef struct {
	char _unused;
} echo_state_t;

static void*
echo_init(const char* api_key, const char* base_url)
{
	(void)api_key;
	(void)base_url;
	return calloc(1, sizeof(echo_state_t));
}

static int
echo_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
	(void)state;
	if (!res) {
		return -1;
	}

	size_t total = 0;
	for (size_t i = 0; i < req->message_count; i++) {
		if (req->messages[i].role && strcmp(req->messages[i].role, "user") == 0 &&
		    req->messages[i].content) {
			total += strlen(req->messages[i].content);
		}
	}
	total += 32;
	res->content = calloc(1, total);
	if (!res->content) {
		return -1;
	}

	int first = 1;
	for (size_t i = 0; i < req->message_count; i++) {
		if (req->messages[i].role && strcmp(req->messages[i].role, "user") == 0 &&
		    req->messages[i].content) {
			if (first) {
				strcat(res->content, "Echo: ");
				first = 0;
			} else {
				strcat(res->content, " ");
			}
			strcat(res->content, req->messages[i].content);
		}
	}
	if (first) {
		strcpy(res->content, "Echo: (no user message)");
	}

	res->prompt_tokens = 10;
	res->completion_tokens = (int)strlen(res->content);
	res->total_tokens = res->prompt_tokens + res->completion_tokens;
	return 0;
}

static void
echo_free(void* state)
{
	free(state);
}

static const csilk_ai_driver_t echo_driver = {
    .name = "echo",
    .init = echo_init,
    .chat = echo_chat,
    .embeddings = NULL,
    .free = echo_free,
};

int
main()
{
	printf("\n===== csilk AI Multi-Provider Example =====\n\n");

	/* Register the custom "echo" driver */
	csilk_ai_register_driver(&echo_driver);

	/* ---- Provider 1: Custom Echo Driver ---- */
	printf("--- Provider: Custom Echo Driver ---\n");
	csilk_ai_t* echo = csilk_ai_new("echo", NULL, NULL);
	if (echo) {
		csilk_ai_message_t msgs[] = {{"user", "Hello from csilk!"},
					     {"user", "Custom provider demo."}};
		csilk_ai_chat_request_t req = {
		    .model = "echo-v1", .messages = msgs, .message_count = 2};
		csilk_ai_chat_response_t res;
		if (csilk_ai_chat(echo, &req, &res) == 0) {
			printf("  Response: %s\n", res.content);
			csilk_ai_chat_response_free(&res);
		}
		csilk_ai_free(echo);
	}

	/* ---- Provider 2: OpenAI-Compatible ---- */
	const char* openai_key = getenv("OPENAI_API_KEY");
	if (openai_key) {
		printf("\n--- Provider: OpenAI-Compatible ---\n");
		const char* openai_base = getenv("OPENAI_API_BASE");
		const char* openai_model = getenv("OPENAI_MODEL");
		if (!openai_model) {
			openai_model = "gpt-4o";
		}

		csilk_ai_t* openai = csilk_ai_new("openai", openai_key, openai_base);
		if (openai) {
			csilk_ai_message_t msgs[] = {{"system", "You are a concise assistant."},
						     {"user", "What is the capital of France?"}};
			csilk_ai_chat_request_t req = {.model = openai_model,
						       .messages = msgs,
						       .message_count = 2,
						       .max_tokens = 100};
			csilk_ai_chat_response_t res;
			if (csilk_ai_chat(openai, &req, &res) == 0) {
				printf("  Response: %s\n", res.content);
				csilk_ai_chat_response_free(&res);

				/* Streaming demo */
				printf("\n--- Streaming Demo ---\n");
				req.stream = true;
				req.on_chunk = (csilk_ai_stream_cb)printf;
				printf("  ");
				csilk_ai_chat(openai, &req, &res);
				printf("\n");
				csilk_ai_chat_response_free(&res);
			} else {
				printf("  Error: %s\n",
				       res.error_message ? res.error_message : "unknown");
				csilk_ai_chat_response_free(&res);
			}
			csilk_ai_free(openai);
		}
	} else {
		printf("\n--- Provider: OpenAI-Compatible (skipped) ---\n");
		printf("  Set OPENAI_API_KEY to test.\n");
	}

	/* ---- Provider 3: Ollama (local) ---- */
	printf("\n--- Provider: Ollama (local) ---\n");
	printf("  Use: csilk_ai_new(\"ollama\", NULL, NULL)\n");
	printf("  Model example: llama3.2\n");
	printf("  Start Ollama: ollama serve\n");
	printf("  (Requires Ollama driver fix — see src/drivers/ai_ollama.c:191)\n");

	/* ---- AI Engine Statistics ---- */
	printf("\n--- AI Engine Statistics ---\n");
	csilk_ai_stats_t stats;
	csilk_ai_get_stats(&stats);
	printf("  Requests: %lu  Tokens: %lu  Errors: %lu\n",
	       (unsigned long)stats.requests_total,
	       (unsigned long)stats.tokens_total,
	       (unsigned long)stats.errors_total);

	char* json_stats = csilk_ai_stats_to_json(&stats);
	if (json_stats) {
		printf("  JSON: %s\n", json_stats);
		free(json_stats);
	}

	printf("\nDone.\n");
	return 0;
}
