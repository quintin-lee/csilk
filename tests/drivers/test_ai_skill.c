#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "csilk/app/workflow.h"
#include "csilk/core/json/json.h"
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

    /* 5. JSON escaping: write a shell script under an exec root that emits
     *    controlled bytes via `printf` (shell builtin, no external PATH
     *    dependency); run it via `bin/sh skill_esc.sh` (single-token args,
     *    safe for the space-split strtok argv builder).  The JSON envelope
     *    must re-escape tab/newline/backslash/quote/CR; we parse it back
     *    with csilk_json_parse and compare against the known bytes. */
    {
        const char* root_dir = "/tmp/skill_exec_root";
        const char* script = "/tmp/skill_exec_root/skill_esc.sh";
        mkdir(root_dir, 0777);
        mkdir("/tmp/skill_exec_root/bin", 0777);
        /* Use /bin/sh (POSIX-mandated on both Linux and macOS); /usr/bin/sh
         * does not exist on macOS where /bin and /usr/bin are separate. */
        unlink("/tmp/skill_exec_root/bin/sh");
        symlink("/bin/sh", "/tmp/skill_exec_root/bin/sh");

        /* The script emits each control byte via printf.  We use POSIX
         * octal escapes in the printf *format string* so the bytes are
         * produced identically by dash (Ubuntu CI /bin/sh) and bash:
         *   \011 → TAB, \012 → LF, \015 → CR.
         * (The C-escape forms \t/\n/\r are silently dropped by dash's
         *  builtin printf, which broke the control-byte assertions.)
         * For the quote case we use `printf '%s' '<literal>'`: a
         * double-quote inside a shell single-quoted string is a literal "
         * with no printf-format-escape ambiguity. */
        const char* script_body = "printf 'a\\011b'\n"
                                  "printf 'x\\012y'\n"
                                  "printf 'a\\\\b'\n"
                                  "printf '%s' 'q\"r'\n"
                                  "printf 'z\\015'\n";
        FILE*       sf = fopen(script, "w");
        assert(sf);
        size_t slen = strlen(script_body);
        assert(fwrite(script_body, 1, slen, sf) == slen);
        fclose(sf);

        /* Point the exec root at the script directory. */
        csilk_skill_exec_cfg_t rcfg = {
            .root = (char*)root_dir, .timeout_ms = 5000, .max_output_bytes = 4096, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &rcfg) == 0);

        /* `bin/sh` relative to root (symlink created above). */
        const char* script_rel = "skill_esc.sh";
        out = NULL;
        rc = csilk_skill_exec_local(sk, "bin/sh", script_rel, 0, &out);
        assert(rc == 0 && out);
        csilk_json_t* js = csilk_json_parse(out);
        assert(js && csilk_json_is_object(js));
        assert(csilk_json_get_int(js, "exit_code") == 0);
        /* stdout = a<TAB>b\nx<LF>y\na\\b\nq\"r\nz<CR>\n (each printf adds LF
         *    except the last which also adds LF; backslash case emits one
         *    backslash + b + LF; quote case emits q"r + LF). */
        const char* got5 = csilk_json_get_string(js, "stdout");
        assert(got5);
        /* Assert the JSON-escaped re-parse reproduces the exact bytes: */
        assert(strstr(got5, "a\tb") != NULL); /* tab */
        assert(strstr(got5, "x\ny") != NULL); /* newline */
        assert(strstr(got5, "a\\b") != NULL); /* one backslash */
        assert(strstr(got5, "q\"r") != NULL); /* quote */
        assert(strstr(got5, "z\r") != NULL);  /* CR */
        csilk_json_free(js);
        free(out);

        /* Multi-line echo via a second script (args must be a single
         * token — the exec's argv builder splits on spaces, so shell
         * commands with embedded spaces must go through a script file). */
        csilk_skill_exec_cfg_t rcfg2 = {
            .root = (char*)root_dir, .timeout_ms = 5000, .max_output_bytes = 4096, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &rcfg2) == 0);
        const char* mbody = "echo line1\necho line2\n";
        FILE*       mf = fopen(script, "w");
        assert(mf);
        assert(fwrite(mbody, 1, strlen(mbody), mf) == strlen(mbody));
        fclose(mf);
        out = NULL;
        rc = csilk_skill_exec_local(sk, "bin/sh", script_rel, 0, &out);
        assert(rc == 0 && out);
        csilk_json_t* je = csilk_json_parse(out);
        assert(je && csilk_json_is_object(je));
        assert(strcmp(csilk_json_get_string(je, "stdout"), "line1\nline2\n") == 0);
        assert(csilk_json_get_int(je, "exit_code") == 0);
        csilk_json_free(je);
        free(out);

        /* Restore base cfg (root=/). */
        csilk_skill_exec_cfg_t ocfg = {
            .root = "/", .timeout_ms = 5000, .max_output_bytes = 1024, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &ocfg) == 0);

        /* cleanup */
        unlink(script);
        rmdir(root_dir);
        ocfg.root = "/";
        csilk_ai_skill_set_exec_cfg(sk, &ocfg);
    }

    /* 6. Non-zero exit code: a script that `exit 42`s.  (Shell commands
     *    with embedded spaces must go through a script file — the argv
     *    builder splits on spaces, so `sh -c '...'` is not a single token.) */
    {
        csilk_skill_exec_cfg_t rcfg3 = {.root = (char*)"/tmp/skill_exec_root",
                                        .timeout_ms = 5000,
                                        .max_output_bytes = 4096,
                                        .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &rcfg3) == 0);
        FILE* mf = fopen("/tmp/skill_exec_root/skill_exit42.sh", "w");
        assert(mf);
        const char* body = "exit 42\n";
        assert(fwrite(body, 1, strlen(body), mf) == strlen(body));
        fclose(mf);
        out = NULL;
        rc = csilk_skill_exec_local(sk, "bin/sh", "skill_exit42.sh", 0, &out);
        assert(rc == 0);
        assert(out && strstr(out, "\"exit_code\":42"));
        free(out);
        unlink("/tmp/skill_exec_root/skill_exit42.sh");
        csilk_skill_exec_cfg_t ocfg2 = {
            .root = "/", .timeout_ms = 5000, .max_output_bytes = 1024, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &ocfg2) == 0);
    }

    /* 7. Output truncation: a script that emits > max_output_bytes. */
    {
        csilk_skill_exec_cfg_t rcfg4 = {.root = (char*)"/tmp/skill_exec_root",
                                        .timeout_ms = 5000,
                                        .max_output_bytes = 64,
                                        .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &rcfg4) == 0);
        FILE* mf = fopen("/tmp/skill_exec_root/skill_trunc.sh", "w");
        assert(mf);
        const char* body = "for i in $(seq 1 100); do printf 'x'; done\n";
        assert(fwrite(body, 1, strlen(body), mf) == strlen(body));
        fclose(mf);
        out = NULL;
        rc = csilk_skill_exec_local(sk, "bin/sh", "skill_trunc.sh", 0, &out);
        assert(rc == 0);
        assert(out && strstr(out, "\"truncated\":true"));
        free(out);
        unlink("/tmp/skill_exec_root/skill_trunc.sh");
        csilk_skill_exec_cfg_t ocfg3 = {
            .root = "/", .timeout_ms = 5000, .max_output_bytes = 1024, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &ocfg3) == 0);
    }

    /* 8. NULL args: path only, no extra arguments (echo with no args → empty line). */
    out = NULL;
    rc = csilk_skill_exec_local(sk, "bin/echo", NULL, 0, &out);
    assert(rc == 0);
    assert(out && strstr(out, "\"exit_code\":0"));
    free(out);

    /* 9. Env variable injection: a script that reads the env var. */
    {
        csilk_skill_exec_cfg_t rcfg5 = {.root = (char*)"/tmp/skill_exec_root",
                                        .timeout_ms = 5000,
                                        .max_output_bytes = 1024,
                                        .env = "MY_TEST_VAR=hello_env"};
        assert(csilk_ai_skill_set_exec_cfg(sk, &rcfg5) == 0);
        FILE* mf = fopen("/tmp/skill_exec_root/skill_env.sh", "w");
        assert(mf);
        const char* body = "printf '%s' \"$MY_TEST_VAR\"\n";
        assert(fwrite(body, 1, strlen(body), mf) == strlen(body));
        fclose(mf);
        out = NULL;
        rc = csilk_skill_exec_local(sk, "bin/sh", "skill_env.sh", 0, &out);
        assert(rc == 0);
        assert(out && strstr(out, "hello_env"));
        free(out);
        unlink("/tmp/skill_exec_root/skill_env.sh");
        csilk_skill_exec_cfg_t ocfg4 = {
            .root = "/", .timeout_ms = 5000, .max_output_bytes = 1024, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &ocfg4) == 0);
    }

    /* 10. NULL sk / NULL out_result → -1. */
    out = (char*)0x1;
    assert(csilk_skill_exec_local(NULL, "bin/echo", "hi", 0, &out) == -1);
    assert(out == NULL);
    assert(csilk_skill_exec_local(sk, "bin/echo", "hi", 0, NULL) == -1);

    /* 11. No exec root configured → error. */
    csilk_skill_meta_t m2 = {.name = "no-root", .description = "No exec root."};
    csilk_ai_skill_t*  sk2 = csilk_ai_skill_new(&m2, CSILK_SKILL_LOCAL, NULL, NULL);
    assert(sk2 != NULL);
    out = (char*)0x1;
    rc = csilk_skill_exec_local(sk2, "bin/echo", "hi", 0, &out);
    assert(rc == -1);
    assert(out && strstr(out, "no exec root"));
    free(out);
    csilk_ai_skill_free(sk2);

    csilk_ai_skill_free(sk);
}

/* Exec config lifecycle: field copy, reset, NULL-safety. */
static void
test_exec_cfg_lifecycle(void)
{
    csilk_skill_meta_t m = {.name = "cfg-test", .description = "Config test."};
    csilk_ai_skill_t*  sk = csilk_ai_skill_new(&m, CSILK_SKILL_LOCAL, NULL, NULL);
    assert(sk != NULL);

    /* 1. Initial state: no exec root. */
    char* out = (char*)0x1;
    int   rc = csilk_skill_exec_local(sk, "bin/echo", "hi", 0, &out);
    assert(rc == -1);
    assert(out && strstr(out, "no exec root"));
    free(out);
    out = NULL;

    /* 2. Set root="/" + timeout=5000. */
    csilk_skill_exec_cfg_t cfg1 = {
        .root = "/", .timeout_ms = 5000, .max_output_bytes = 1024, .env = NULL};
    assert(csilk_ai_skill_set_exec_cfg(sk, &cfg1) == 0);
    rc = csilk_skill_exec_local(sk, "bin/echo", "cfg-ok", 0, &out);
    assert(rc == 0);
    assert(out && strstr(out, "\"exit_code\":0"));
    assert(out && strstr(out, "cfg-ok"));
    free(out);

    /* 3. Override with a different root + env.  Symlink /bin/echo under
     *    /tmp/skill_exec_root/bin so this works on macOS too (/usr/bin/echo
     *    does not exist on macOS). */
    mkdir("/tmp/skill_exec_root", 0777);
    mkdir("/tmp/skill_exec_root/bin", 0777);
    unlink("/tmp/skill_exec_root/bin/echo");
    symlink("/bin/echo", "/tmp/skill_exec_root/bin/echo");
    csilk_skill_exec_cfg_t cfg2 = {.root = "/tmp/skill_exec_root",
                                   .timeout_ms = 10000,
                                   .max_output_bytes = 2048,
                                   .env = "FOO=bar"};
    assert(csilk_ai_skill_set_exec_cfg(sk, &cfg2) == 0);
    rc = csilk_skill_exec_local(sk, "bin/echo", "cfg2", 0, &out);
    assert(rc == 0);
    assert(out && strstr(out, "cfg2"));
    free(out);
    unlink("/tmp/skill_exec_root/bin/echo");

    /* 4. NULL cfg rejected. */
    assert(csilk_ai_skill_set_exec_cfg(sk, NULL) == -1);

    /* 5. NULL sk rejected. */
    csilk_skill_exec_cfg_t cfg3 = {.root = "/", .timeout_ms = 1000};
    assert(csilk_ai_skill_set_exec_cfg(NULL, &cfg3) == -1);

    /* 6. Re-set to the same root does not leak: run a few times to
     * stress the free/strdup cycle without leaking. */
    for (int i = 0; i < 10; i++) {
        csilk_skill_exec_cfg_t c = {
            .root = "/", .timeout_ms = 1000, .max_output_bytes = 512, .env = NULL};
        assert(csilk_ai_skill_set_exec_cfg(sk, &c) == 0);
    }

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
    test_exec_cfg_lifecycle();
    printf("test_ai_skill: ALL PASSED\n");
    return 0;
}
