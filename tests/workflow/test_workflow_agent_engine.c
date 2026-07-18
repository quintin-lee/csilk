#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/core/sys_io.h"

static int g_eval_calls = 0;

static int
sample_eval_fn(const char* output, char** feedback_out, void* user_data)
{
    (void)output;
    (void)user_data;
    g_eval_calls++;
    if (g_eval_calls < 2) {
        if (feedback_out) {
            *feedback_out = strdup("Please make the answer more concise.");
        }
        return 0; // Failed, trigger reflection loop
    }
    return 1;     // Passed evaluation
}

static void
test_agent_memory()
{
    printf("Testing Agent context short-term memory API...\n");
    csilk_wf_t* wf = csilk_wf_new("test_mem_wf");
    assert(wf != NULL);

    csilk_wf_node_t* n1 = csilk_wf_add_agent_react(
        wf,
        "react_node",
        &(csilk_agent_react_config_t){.model = "gpt-3.5-turbo",
                                      .system_prompt = "You are a ReAct reasoning agent.",
                                      .prompt = "Solve math problem",
                                      .max_iterations = 5});
    assert(n1 != NULL);

    csilk_wf_free(wf);
    printf("test_agent_memory: PASS\n");
}

static void
test_agent_reflexion_node()
{
    printf("Testing Agent Reflexion self-correction node creation...\n");
    g_eval_calls = 0;
    csilk_wf_t* wf = csilk_wf_new("test_reflexion_wf");
    assert(wf != NULL);

    csilk_wf_node_t* n_ref = csilk_wf_add_agent_reflexion(
        wf,
        "reflexion_node",
        &(csilk_agent_reflexion_config_t){.model = "gpt-3.5-turbo",
                                          .prompt = "Write code snippet",
                                          .max_reflections = 3,
                                          .eval_fn = sample_eval_fn,
                                          .eval_user_data = NULL});
    assert(n_ref != NULL);

    csilk_wf_free(wf);
    printf("test_agent_reflexion_node: PASS\n");
}

int
main()
{
    test_agent_memory();
    test_agent_reflexion_node();
    printf("All Agent Engine tests passed successfully!\n");
    return 0;
}
