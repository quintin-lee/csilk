#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

static int g_done = 0;
static char* g_final_output = NULL;

/* --- Handlers --- */

csilk_data_t*
json_producer_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	(void)user_data;
	printf("[Producer] Producing JSON data\n");
	const char* json = "{\"user\": {\"name\": \"Quintin\", \"details\": {\"age\": 30, "
			   "\"active\": true}}}";
	return csilk_wf_data_new(ctx, "application/json", csilk_wf_strdup(ctx, json));
}

/* --- Mock AI for testing prompt resolution --- */
static int
mock_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
	(void)state;
	// The prompt is the last message
	const char* prompt = req->messages[req->message_count - 1].content;
	printf("[MockAI] Received resolved prompt: %s\n", prompt);

	memset(res, 0, sizeof(*res));
	res->content = strdup(prompt); // Echo the prompt back
	return 0;
}

static void*
mock_init(const char* key, const char* url)
{
	(void)key;
	(void)url;
	return (void*)1;
}
static void
mock_free(void* state)
{
	(void)state;
}

static csilk_ai_driver_t mock_driver = {
    .name = "openai", .init = mock_init, .chat = mock_chat, .free = mock_free};

void
on_jsonpath_complete(csilk_data_t* result)
{
	if (result && result->value) {
		g_final_output = strdup((char*)result->value);
	}
	g_done = 1;
}

void
test_workflow_jsonpath_injection()
{
	printf("Testing JSONPath variable injection...\n");
	g_done = 0;
	if (g_final_output) {
		free(g_final_output);
		g_final_output = NULL;
	}

	// Overwrite the real OpenAI driver with our mock
	csilk_ai_register_driver(&mock_driver);
	setenv("AGENT_API_KEY", "mock", 1);

	csilk_wf_t* wf = csilk_wf_new("jsonpath_wf");

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "producer", json_producer_handler, NULL);

	csilk_wf_node_t* n2 = csilk_wf_add_ai(
	    wf,
	    "consumer",
	    &(csilk_ai_config_t){.prompt = "Hello {{producer.value.user.name}}! Age: "
					   "{{producer.value.user.details.age}}, Active: "
					   "{{producer.value.user.details.active}}"});

	csilk_wf_bind(n1, n2);
	csilk_wf_node_set_entry(n1, 1);

	csilk_wf_run(wf, NULL, on_jsonpath_complete);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(g_done == 1);
	assert(g_final_output != NULL);

	printf("Final Resolved Prompt: %s\n", g_final_output);

	assert(strstr(g_final_output, "Hello Quintin!") != NULL);
	assert(strstr(g_final_output, "Age: 30") != NULL);
	assert(strstr(g_final_output, "Active: true") != NULL);

	csilk_wf_free(wf);
	if (g_final_output) {
		free(g_final_output);
	}
	printf("test_workflow_jsonpath_injection: PASS\n");
}

int
main()
{
	test_workflow_jsonpath_injection();
	return 0;
}
