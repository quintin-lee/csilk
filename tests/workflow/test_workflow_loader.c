#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"

csilk_data_t*
t_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    return nullptr;
}

static void
test_json_basic()
{
    printf("Testing JSON loader basic...\n");

    csilk_wf_register_handler("echo", t_handler);

    const char* json = "{"
                       "\"name\": \"TestWF\","
                       "\"steps\": ["
                       "  {\"id\": \"step1\", \"handler\": \"echo\", \"entry\": true},"
                       "  {\"id\": \"step2\", \"type\": \"ai\", \"config\": {\"model\": "
                       "\"gpt-4\", \"prompt\": \"hello\"}}"
                       "],"
                       "\"connections\": ["
                       "  {\"from\": \"step1\", \"to\": \"step2\"}"
                       "]"
                       "}";

    csilk_wf_t* wf = csilk_wf_from_json(json);
    assert(wf != nullptr);

    char* mermaid = csilk_wf_to_mermaid(wf);
    assert(mermaid != nullptr);
    assert(strstr(mermaid, "\"step1\" --> \"step2\"") != nullptr);

    free(mermaid);
    csilk_wf_free(wf);
    printf("  passed\n");
}

static void
test_json_null_empty()
{
    printf("Testing JSON loader null/empty input...\n");

    assert(csilk_wf_from_json(nullptr) == nullptr);
    assert(csilk_wf_from_json("") == nullptr);
    assert(csilk_wf_from_json("not valid json at all") == nullptr);

    printf("  passed\n");
}

static void
test_json_missing_fields()
{
    printf("Testing JSON loader edge cases...\n");

    /* no name — creates workflow with default empty name */
    csilk_wf_t* wf = csilk_wf_from_json("{\"steps\":[], \"connections\":[]}");
    assert(wf != nullptr);
    csilk_wf_free(wf);

    /* no steps — creates workflow with no nodes */
    wf = csilk_wf_from_json("{\"name\":\"test\", \"connections\":[]}");
    assert(wf != nullptr);
    char* m = csilk_wf_to_mermaid(wf);
    assert(m != nullptr);
    free(m);
    csilk_wf_free(wf);

    /* no connections — creates workflow with nodes only */
    csilk_wf_register_handler("nop", t_handler);
    wf = csilk_wf_from_json("{\"name\":\"solo\", \"steps\":["
                            "{\"id\":\"n1\", \"handler\":\"nop\", \"entry\":true}"
                            "], \"connections\":[]}");
    assert(wf != nullptr);
    m = csilk_wf_to_mermaid(wf);
    assert(m != nullptr);
    free(m);
    csilk_wf_free(wf);

    printf("  passed\n");
}

static void
test_mermaid_null()
{
    printf("Testing Mermaid export null safety...\n");

    char* m = csilk_wf_to_mermaid(nullptr);
    assert(m == nullptr);

    printf("  passed\n");
}

static void
test_json_parallel_steps()
{
    printf("Testing JSON loader parallel steps...\n");

    csilk_wf_register_handler("entry", t_handler);
    csilk_wf_register_handler("worker", t_handler);

    const char* json = "{"
                       "\"name\": \"Parallel\","
                       "\"steps\": ["
                       "  {\"id\": \"start\", \"handler\": \"entry\", \"entry\": true},"
                       "  {\"id\": \"a\", \"handler\": \"worker\"},"
                       "  {\"id\": \"b\", \"handler\": \"worker\"},"
                       "  {\"id\": \"end\", \"handler\": \"entry\"}"
                       "],"
                       "\"connections\": ["
                       "  {\"from\": \"start\", \"to\": \"a\"},"
                       "  {\"from\": \"start\", \"to\": \"b\"},"
                       "  {\"from\": \"a\", \"to\": \"end\"},"
                       "  {\"from\": \"b\", \"to\": \"end\"}"
                       "]"
                       "}";

    csilk_wf_t* wf = csilk_wf_from_json(json);
    assert(wf != nullptr);

    char* mermaid = csilk_wf_to_mermaid(wf);
    assert(mermaid != nullptr);

    /* Diamond pattern: start→a→end and start→b→end */
    assert(strstr(mermaid, "\"start\" --> \"a\"") != nullptr);
    assert(strstr(mermaid, "\"start\" --> \"b\"") != nullptr);
    assert(strstr(mermaid, "\"a\" --> \"end\"") != nullptr);
    assert(strstr(mermaid, "\"b\" --> \"end\"") != nullptr);

    free(mermaid);
    csilk_wf_free(wf);
    printf("  passed\n");
}

static void
test_json_entry_point()
{
    printf("Testing JSON loader entry point...\n");

    csilk_wf_register_handler("dummy", t_handler);

    const char* json = "{"
                       "\"name\": \"Entry\","
                       "\"steps\": ["
                       "  {\"id\": \"first\", \"handler\": \"dummy\", \"entry\": true},"
                       "  {\"id\": \"second\", \"handler\": \"dummy\"}"
                       "],"
                       "\"connections\": ["
                       "  {\"from\": \"first\", \"to\": \"second\"}"
                       "]"
                       "}";

    csilk_wf_t* wf = csilk_wf_from_json(json);
    assert(wf != nullptr);

    csilk_wf_node_t* entry = csilk_wf_get_node(wf, "first");
    assert(entry != nullptr);

    csilk_wf_node_t* second = csilk_wf_get_node(wf, "second");
    assert(second != nullptr);

    csilk_wf_free(wf);
    printf("  passed\n");
}

int
main()
{
    test_json_basic();
    test_json_null_empty();
    test_json_missing_fields();
    test_mermaid_null();
    test_json_parallel_steps();
    test_json_entry_point();
    printf("All workflow loader tests passed!\n");
    return 0;
}
