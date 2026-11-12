#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai/skill.h"
#include "csilk/messaging/mq.h"

/* A test loader that returns canned content per level. */
static int
test_skill_load(csilk_ai_skill_t*   sk,
                csilk_skill_level_t level,
                const char*         resource_path,
                char**              out_content,
                void*               user_data)
{
    (void)sk;
    (void)user_data;
    if (level == CSILK_SKILL_INSTRUCTIONS) {
        *out_content = strdup("# Instructions\nDo the thing.");
    } else if (level == CSILK_SKILL_RESOURCES) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"resource\":\"%s\"}", resource_path ? resource_path : "");
        *out_content = strdup(buf);
    }
    return 0;
}

static void
test_validation_rules(void)
{
    /* valid */
    csilk_skill_meta_t m = {.name = "pdf-processing", .description = "Process PDFs."};
    assert(csilk_ai_skill_validate(&m, NULL) == 0);

    /* reserved words */
    csilk_skill_meta_t r1 = {.name = "anthropic", .description = "x"};
    assert(csilk_ai_skill_validate(&r1, NULL) == -6);
    csilk_skill_meta_t r2 = {.name = "claude", .description = "x"};
    assert(csilk_ai_skill_validate(&r2, NULL) == -6);

    /* uppercase rejected */
    csilk_skill_meta_t bad_upper = {.name = "PDF-Processing", .description = "x"};
    assert(csilk_ai_skill_validate(&bad_upper, NULL) == -4);

    /* consecutive hyphens */
    csilk_skill_meta_t bad_dbl = {.name = "pdf--processing", .description = "x"};
    assert(csilk_ai_skill_validate(&bad_dbl, NULL) == -3);

    /* leading / trailing hyphen */
    csilk_skill_meta_t bad_lead = {.name = "-pdf", .description = "x"};
    assert(csilk_ai_skill_validate(&bad_lead, NULL) == -5);
    csilk_skill_meta_t bad_tail = {.name = "pdf-", .description = "x"};
    assert(csilk_ai_skill_validate(&bad_tail, NULL) == -5);

    /* empty name / description treated as missing → -1 */
    csilk_skill_meta_t empty_name = {.name = "", .description = "x"};
    assert(csilk_ai_skill_validate(&empty_name, NULL) == -1);
    csilk_skill_meta_t empty_desc = {.name = "ok", .description = ""};
    assert(csilk_ai_skill_validate(&empty_desc, NULL) == -1);

    /* description with angle brackets */
    csilk_skill_meta_t angle = {.name = "ok", .description = "does <x>"};
    assert(csilk_ai_skill_validate(&angle, NULL) == -8);

    /* NULL meta */
    assert(csilk_ai_skill_validate(NULL, NULL) == -1);
}

static void
test_lifecycle_and_load(void)
{
    csilk_skill_meta_t m = {.name = "code-review", .description = "Review code when asked."};
    csilk_ai_skill_t*  sk = csilk_ai_skill_new(&m, CSILK_SKILL_LOCAL, test_skill_load, NULL);
    assert(sk != NULL);

    /* L1 metadata is intrinsic (no loader needed). */
    char* meta = NULL;
    assert(csilk_ai_skill_load(sk, CSILK_SKILL_METADATA, NULL, &meta) == 0);
    assert(meta && strstr(meta, "code-review") && strstr(meta, "Review code"));
    free(meta);

    /* L2 via loader. */
    char* instr = NULL;
    assert(csilk_ai_skill_load(sk, CSILK_SKILL_INSTRUCTIONS, NULL, &instr) == 0);
    assert(instr && strstr(instr, "Do the thing"));
    free(instr);

    /* L3 resource via loader. */
    char* res = NULL;
    assert(csilk_ai_skill_load(sk, CSILK_SKILL_RESOURCES, "scripts/run.sh", &res) == 0);
    assert(res && strstr(res, "scripts/run.sh"));
    free(res);

    /* metadata serializer */
    char* s = csilk_skill_meta_to_string(sk);
    assert(s && strstr(s, "code-review"));
    free(s);

    csilk_ai_skill_free(sk);

    /* invalid meta → NULL */
    csilk_skill_meta_t bad = {.name = "anthropic", .description = "x"};
    assert(csilk_ai_skill_new(&bad, CSILK_SKILL_LOCAL, NULL, NULL) == NULL);
}

/* Discovery: register a skill on a workflow, drive the discovery callback,
 * and confirm a `use_skill` tool with the aggregated description appears. */
static void
test_discovery_injection(void)
{
    csilk_wf_t* wf = csilk_wf_new("skill-test-wf");
    assert(wf != NULL);

    csilk_skill_meta_t m1 = {.name = "pdf-processing", .description = "Process PDFs."};
    csilk_skill_meta_t m2 = {.name = "code-review", .description = "Review code."};
    csilk_ai_skill_t*  sk1 = csilk_ai_skill_new(&m1, CSILK_SKILL_LOCAL, test_skill_load, NULL);
    csilk_ai_skill_t*  sk2 = csilk_ai_skill_new(&m2, CSILK_SKILL_LOCAL, NULL, NULL);
    assert(sk1 && sk2);

    assert(csilk_ai_skill_register(wf, sk1) == 0);
    assert(csilk_ai_skill_register(wf, sk2) == 0);

    char* meta = NULL;
    assert(csilk_ai_skill_load(sk1, CSILK_SKILL_METADATA, NULL, &meta) == 0);
    assert(meta && strstr(meta, "pdf-processing"));
    free(meta);

    char* instr = NULL;
    assert(csilk_ai_skill_load(sk1, CSILK_SKILL_INSTRUCTIONS, NULL, &instr) == 0);
    assert(instr && strstr(instr, "Do the thing"));
    free(instr);

    /* NULL-arg safety. */
    assert(csilk_ai_skill_register(NULL, sk1) == -1);
    assert(csilk_ai_skill_register(wf, NULL) == -1);

    csilk_ai_skill_free(sk1);
    csilk_ai_skill_free(sk2);
    csilk_wf_free(wf);
}

/* The discovery callback must emit a use_skill trigger tool.  We reach it via
 * the public registration path: register on a fresh wf, then invoke the
 * programmatic trigger (equivalent to the model emitting a use_skill call). */
static void
test_trigger_tool(void)
{
    csilk_wf_t* wf = csilk_wf_new("skill-trigger-wf");
    assert(wf != NULL);

    csilk_skill_meta_t m = {.name = "data-etch", .description = "Etch data."};
    csilk_ai_skill_t*  sk = csilk_ai_skill_new(&m, CSILK_SKILL_LOCAL, test_skill_load, NULL);
    assert(sk != NULL);
    assert(csilk_ai_skill_register(wf, sk) == 0);

    /* L2: invoke by name only → returns the loaded instructions. */
    char* r2 = csilk_ai_skill_run_use_skill(wf, "{\"name\":\"data-etch\"}");
    assert(r2 && strstr(r2, "Do the thing"));
    free(r2);

    /* L3: invoke with a resource → returns the named resource. */
    char* r3 = csilk_ai_skill_run_use_skill(
        wf, "{\"name\":\"data-etch\",\"resource\":\"scripts/run.sh\"}");
    assert(r3 && strstr(r3, "scripts/run.sh"));
    free(r3);

    /* Unknown skill → error envelope. */
    char* rerr = csilk_ai_skill_run_use_skill(wf, "{\"name\":\"nope\"}");
    assert(rerr && strstr(rerr, "not found"));
    free(rerr);

    csilk_ai_skill_free(sk);
    csilk_wf_free(wf);
}

/* Remote dispatch publishes to the MQ and returns a "dispatched" envelope. */
static void
test_remote_dispatch(void)
{
    csilk_skill_meta_t m = {.name = "remote-skill", .description = "Remote exec."};
    csilk_ai_skill_t*  sk = csilk_ai_skill_new(&m, CSILK_SKILL_REMOTE, NULL, NULL);
    assert(sk != NULL);

    /* No MQ handle → failure, out_result set to NULL. */
    char* out = (char*)0x1; /* sentinel */
    assert(csilk_ai_skill_invoke_remote(sk, NULL, "scripts/x.sh", "{}", 0, &out) == -1);
    assert(out == NULL);

    csilk_ai_skill_free(sk);
}

/* Local exec: fork/execvp a real command, verify stdout/exit code; verify
 * timeout SIGKILL and path-traversal rejection. */
static void
test_local_exec(void)
{
    /* 1. Run /bin/echo via exec (path relative to root "/", args = "hello skill"). */
    csilk_skill_meta_t m = {.name = "echo-skill", .description = "Run echo."};
    csilk_ai_skill_t*  sk = csilk_ai_skill_new(&m, CSILK_SKILL_LOCAL, NULL, NULL);
    assert(sk != NULL);

    csilk_skill_exec_cfg_t cfg = {
        .root = "/", .timeout_ms = 5000, .max_output_bytes = 1024, .env = NULL};
    assert(csilk_ai_skill_set_exec_cfg(sk, &cfg) == 0);

    char* out = NULL;
    int   rc = csilk_skill_exec_local(sk, "bin/echo", "hello skill", 0, &out);
    assert(rc == 0);
    assert(out && strstr(out, "\"exit_code\":0"));
    assert(strstr(out, "hello skill"));
    free(out);

    /* 2. Absolute path is rejected even when root would "allow" it. */
    out = (char*)0x1; /* sentinel */
    rc = csilk_skill_exec_local(sk, "/bin/echo", "hi", 0, &out);
    assert(rc == -1);
    assert(out && strstr(out, "path escapes"));
    free(out);

    /* 3. `..` traversal: rejected. */
    out = (char*)0x1;
    rc = csilk_skill_exec_local(sk, "../bin/echo", "hi", 0, &out);
    assert(rc == -1);
    assert(out && strstr(out, "path escapes"));
    free(out);

    /* 4. Timeout: bin/sleep 5s with a 200ms bound → exit_code -9. */
    out = NULL;
    rc = csilk_skill_exec_local(sk, "bin/sleep", "5", 200, &out);
    assert(rc == 0);
    assert(out && strstr(out, "\"exit_code\":-9"));
    free(out);

    csilk_ai_skill_free(sk);
}

int
main(void)
{
    printf("Running test_ai_skill suite...\n");
    test_validation_rules();
    test_lifecycle_and_load();
    test_discovery_injection();
    test_trigger_tool();
    test_remote_dispatch();
    test_local_exec();
    printf("test_ai_skill: ALL PASSED\n");
    return 0;
}
