#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/core/sys_io.h"

static csilk_hitl_decision_t
sample_hitl_eval_fn(const char* output, void* user_data)
{
    (void)output;
    (void)user_data;
    return CSILK_HITL_REQUIRES_HUMAN;
}

static void
test_agent_hitl_node()
{
    printf("Testing Agent Human-in-the-Loop (HITL) node creation & interactive marking...\n");
    csilk_wf_t* wf = csilk_wf_new("hitl_wf");
    assert(wf != NULL);

    csilk_wf_node_t* hitl_node =
        csilk_wf_add_agent_hitl(wf,
                                "hitl_1",
                                &(csilk_agent_hitl_config_t){.model = "gpt-3.5-turbo",
                                                             .prompt = "Execute high risk action",
                                                             .eval_fn = sample_hitl_eval_fn,
                                                             .eval_user_data = NULL});
    assert(hitl_node != NULL);

    csilk_wf_free(wf);
    printf("test_agent_hitl_node: PASS\n");
}

int
main()
{
    test_agent_hitl_node();
    printf("All Agent HITL tests passed successfully!\n");
    return 0;
}
