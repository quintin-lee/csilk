#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/workflow/workflow.h"

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

/* Exercise the loader's condition / loop / join=or / on_error / unknown-handler
 * branches (Pass 2 + Pass 3 in csilk_wf_from_json). */
static void
test_json_condition_loop_join_onerror()
{
    printf("Testing JSON loader condition/loop/join/on_error branches...\n");

    csilk_wf_register_handler("s", t_handler);

    const char* json = "{"
                       "\"name\": \"Branches\","
                       "\"steps\": ["
                       "  {\"id\": \"a\", \"handler\": \"s\", \"entry\": true, \"join\": \"or\"},"
                       "  {\"id\": \"b\", \"handler\": \"s\"},"
                       "  {\"id\": \"c\", \"handler\": \"s\", \"on_error\": \"b\"},"
                       "  {\"id\": \"d\", \"handler\": \"no_such_handler\"}"
                       "],"
                       "\"connections\": ["
                       "  {\"from\": \"a\", \"to\": \"b\", \"condition\": \"$ok\"},"
                       "  {\"from\": \"b\", \"to\": \"a\", \"loop\": true},"
                       "  {\"from\": \"ghost\", \"to\": \"a\"},"
                       "  {\"from\": \"a\"}"
                       "]"
                       "}";

    csilk_wf_t* wf = csilk_wf_from_json(json);
    assert(wf != nullptr);

    /* Confirm the nodes and their attributes survived loading. */
    assert(csilk_wf_get_node(wf, "a") != nullptr);
    assert(csilk_wf_get_node(wf, "b") != nullptr);
    /* The unknown handler 'd' must NOT produce a node. */
    assert(csilk_wf_get_node(wf, "d") == nullptr);

    char* m = csilk_wf_to_mermaid(wf);
    assert(m != nullptr);
    /* 'a' and 'b' are both emitted as nodes in the mermaid graph. */
    assert(strstr(m, "\"a\"") != nullptr);
    assert(strstr(m, "\"b\"") != nullptr);
    free(m);
    csilk_wf_free(wf);
    printf("  passed\n");
}

/* Exercise csilk_wf_load_yaml against a real nested YAML document (a
 * sequence item that is itself a mapping). This is the case that used to
 * trigger a heap-use-after-free in parse_yaml_file (nested containers were
 * absorbed into the parent doc while the freed standalone handle stayed on
 * the stack). */
static void
test_yaml_loader(void)
{
    printf("Testing YAML workflow loader (nested + missing)...\n");

    csilk_wf_register_handler("echo", t_handler);

    const char* path = "test_wf_loader_sample.yaml";
    FILE*       f = fopen(path, "w");
    assert(f != nullptr);
    fprintf(f,
            "name: YamlWF\n"
            "steps:\n"
            "  - id: s1\n"
            "    handler: echo\n"
            "    entry: true\n"
            "  - id: s2\n"
            "    handler: echo\n"
            "connections:\n"
            "  - from: s1\n"
            "    to: s2\n");
    fclose(f);

    csilk_wf_t* wf = csilk_wf_load_yaml(path);
    assert(wf != nullptr);
    /* Both nested step nodes must survive into the workflow. */
    assert(csilk_wf_get_node(wf, "s1") != nullptr);
    assert(csilk_wf_get_node(wf, "s2") != nullptr);
    char* m = csilk_wf_to_mermaid(wf);
    assert(m != nullptr);
    assert(strstr(m, "\"s1\" --> \"s2\"") != nullptr);
    free(m);
    csilk_wf_free(wf);
    remove(path);

    /* Missing file -> NULL (parse_yaml_file fopen failure branch). */
    assert(csilk_wf_load_yaml("definitely_not_there_xyz.yaml") == nullptr);

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
    test_json_condition_loop_join_onerror();
    test_yaml_loader();
    printf("All workflow loader tests passed!\n");
    return 0;
}
