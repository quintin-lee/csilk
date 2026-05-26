/**
 * @file example_ai.c
 * @brief Example usage of the csilk AI unified interface.
 */

#include <stdio.h>
#include <stdlib.h>

#include "csilk.h"
#include "csilk_ai.h"

int main() {
  const char* api_key = getenv("OPENAI_API_KEY");
  if (!api_key) {
    printf("Please set OPENAI_API_KEY environment variable to run this example.\n");
    return 0;
  }

  /* Create an OpenAI AI instance */
  csilk_ai_t* ai = csilk_ai_new("openai", api_key, NULL);
  if (!ai) {
    fprintf(stderr, "Failed to initialize OpenAI driver\n");
    return 1;
  }

  /* Prepare a chat request */
  csilk_ai_message_t messages[] = {
    {"system", "You are a helpful assistant."},
    {"user", "Hello! Can you introduce the csilk C web framework in one sentence?"}
  };

  csilk_ai_chat_request_t req = {
    .model = "gpt-3.5-turbo",
    .messages = messages,
    .message_count = 2,
    .temperature = 0.7,
    .max_tokens = 100
  };

  /* Perform the chat completion */
  csilk_ai_chat_response_t res;
  printf("Calling OpenAI API...\n");
  if (csilk_ai_chat(ai, &req, &res) == 0) {
    printf("\nAI Response:\n%s\n", res.content);
    printf("\nUsage: prompt=%d, completion=%d, total=%d\n", 
           res.prompt_tokens, res.completion_tokens, res.total_tokens);
    
    csilk_ai_chat_response_free(&res);
  } else {
    fprintf(stderr, "AI chat call failed\n");
  }

  csilk_ai_free(ai);
  return 0;
}
