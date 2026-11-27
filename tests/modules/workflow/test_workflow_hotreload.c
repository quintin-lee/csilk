#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"

static void
test_workflow_manager_register_and_reload(void)
{
    csilk_wf_manager_t* mgr = csilk_wf_manager_new();
    assert(mgr != nullptr);

    csilk_wf_t* wf1 = csilk_wf_new("v1");
    assert(wf1 != nullptr);

    int res = csilk_wf_manager_register(mgr, "test_wf", wf1);
    assert(res == 0);

    csilk_wf_t* active1 = csilk_wf_manager_get(mgr, "test_wf");
    assert(active1 == wf1);

    csilk_wf_t* wf2 = csilk_wf_new("v2");
    assert(wf2 != nullptr);

    res = csilk_wf_manager_reload(mgr, "test_wf", wf2);
    assert(res == 0);

    csilk_wf_t* active2 = csilk_wf_manager_get(mgr, "test_wf");
    assert(active2 == wf2);

    csilk_wf_manager_free(mgr);
    printf("test_workflow_manager_register_and_reload passed\n");
}

int
main(void)
{
    test_workflow_manager_register_and_reload();
    printf("All test_workflow_hotreload tests passed!\n");
    return 0;
}
