#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"

static void
test_dsl_json_parsing(void)
{
    const char* dsl_json =
        "{\n"
        "  \"name\": \"test_pipeline\",\n"
        "  \"budget\": {\"max_tokens\": 4096},\n"
        "  \"nodes\": [\n"
        "    {\"id\": \"node1\", \"type\": \"ai_chat\", \"config\": {\"model\": "
        "\"gpt-4o-mini\"}},\n"
        "    {\"id\": \"node2\", \"type\": \"agent_react\", \"depends_on\": [\"node1\"]}\n"
        "  ]\n"
        "}";

    char        err_buf[256] = {0};
    csilk_wf_t* wf = csilk_wf_from_json_ext(dsl_json, err_buf, sizeof(err_buf));
    assert(wf != nullptr);

    char* exported = csilk_wf_to_json(wf);
    assert(exported != nullptr);
    assert(strstr(exported, "test_pipeline") != nullptr);

    free(exported);
    csilk_wf_free(wf);
    printf("test_dsl_json_parsing passed\n");
}

int
main(void)
{
    test_dsl_json_parsing();
    printf("All test_workflow_dsl tests passed!\n");
    return 0;
}
