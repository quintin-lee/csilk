/**
 * @file example_ai.c
 * @brief Example usage of the csilk AI unified interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_ai.h"

static void on_stream_chunk(const char *chunk, void *user_data) {
  (void)user_data;
  printf("%s", chunk);
  fflush(stdout);
}

int main() {
  const char* api_key = getenv("AGENT_API_KEY");
  const char* api_base = getenv("AGENT_API_BASE");
  const char* model_name = getenv("AGENT_MODEL_NAME");

  if (!api_key) {
    printf("Please set AGENT_API_KEY environment variable to run this example.\n");
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
  csilk_ai_message_t messages[] = {
    {"system", "You are a helpful assistant."},
    {"user", "介绍一下自己，并用一句话总结 C 语言的优势。"}
  };

  /* 1. Non-streaming call with enriched parameters */
  csilk_ai_chat_request_t req = {
    .model = model_name,
    .messages = messages,
    .message_count = 2,
    .temperature = 0.7,
    .top_p = 0.9,
    .max_tokens = 512,
    .user = "csilk-test-user"
  };

  csilk_ai_chat_response_t res;
  printf("--- [1] Non-streaming call ---\n");
  if (csilk_ai_chat(ai, &req, &res) == 0) {
    printf("AI Response:\n%s\n", res.content);
    printf("\nUsage: prompt=%d, completion=%d, total=%d\n\n", 
           res.prompt_tokens, res.completion_tokens, res.total_tokens);
    csilk_ai_chat_response_free(&res);
  } else {
    fprintf(stderr, "Non-streaming chat call failed: %s\n", 
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
    fprintf(stderr, "Streaming chat call failed: %s\n", 
            res.error_message ? res.error_message : "Unknown error");
    csilk_ai_chat_response_free(&res);
  }

  csilk_ai_free(ai);
  return 0;
}

