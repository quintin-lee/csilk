#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "cJSON.h"
#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

static int g_tool_called = 0;
static int g_chat_turns = 0;

/* --- Mock Tool --- */
char* mock_get_weather(const char* args_json, void* user_data) {
  (void)user_data;
  printf("[MockTool] Calling get_weather with args: %s\n", args_json);
  g_tool_called = 1;
  return strdup("{\"temp\": 72, \"unit\": \"F\"}");
}

/* --- Mock AI Driver --- */
static int mock_chat(void* state, const csilk_ai_chat_request_t* req,
                     csilk_ai_chat_response_t* res) {
  (void)state;
  g_chat_turns++;
  printf("[MockAI] Turn %d\n", g_chat_turns);

  memset(res, 0, sizeof(*res));

  if (g_chat_turns == 1) {
    // First turn: Request tool call
    res->content = strdup("Let me check the weather.");
    res->tool_calls = calloc(1, sizeof(csilk_ai_tool_call_t));
    res->tool_calls[0].id = strdup("call_123");
    res->tool_calls[0].name = strdup("get_weather");
    res->tool_calls[0].arguments = strdup("{\"location\": \"San Francisco\"}");
    res->tool_call_count = 1;
  } else {
    // Second turn: Return final answer
    res->content = strdup("The weather is 72 degrees.");
  }
  return 0;
}

static void* mock_init(const char* key, const char* url) {
  (void)key;
  (void)url;
  return (void*)1;
}
static void mock_free(void* state) { (void)state; }

static csilk_ai_driver_t mock_driver = {
    .name = "openai",  // Override openai for testing without network
    .init = mock_init,
    .chat = mock_chat,
    .free = mock_free};

void test_workflow_autonomous_tools() {
  printf("Testing autonomous tool-calling...\n");
  g_tool_called = 0;
  g_chat_turns = 0;

  // Register mock driver (overwriting the real one)
  csilk_ai_register_driver(&mock_driver);
  setenv("AGENT_API_KEY", "mock-key", 1);

  csilk_wf_t* wf = csilk_wf_new("agent_wf");

  // 1. Register tool
  csilk_wf_register_tool(wf, "get_weather", "Get current weather",
                         "{\"type\":\"object\",\"properties\":{}}",
                         mock_get_weather, NULL);

  // 2. Add AI node
  csilk_wf_node_t* n1 = csilk_wf_add_ai(
      wf, "agent",
      &(csilk_ai_config_t){.model = "gpt-4", .prompt = "What is the weather?"});
  csilk_wf_node_set_entry(n1, 1);

  // 3. Run
  csilk_wf_run(wf, NULL, NULL);

  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  // Verify results
  assert(g_tool_called == 1);
  assert(g_chat_turns == 2);

  csilk_wf_free(wf);
  printf("test_workflow_autonomous_tools: PASS\n");
}

int main() {
  test_workflow_autonomous_tools();
  return 0;
}
