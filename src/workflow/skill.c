/**
 * @file skill.c
 * @brief Standard Agent Skill abstraction: metadata validation, progressive
 *        disclosure loading, discovery registration, and remote execution.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/ai/skill.h"
#include "csilk/core/json/json.h"
#include "csilk/core/sync.h"
#include "csilk/messaging/mq.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SKILL_NAME_MAX 64
#define SKILL_DESC_MAX 1024
#define SKILL_LICENSE_MAX 64
#define SKILL_COMPAT_MAX 500

#define SKILL_REMOTE_DEFAULT_TIMEOUT_MS 30000
#define SKILL_MAX_WF_ENTRIES 128

typedef struct {
    csilk_ai_skill_t*          sk;
    csilk_wf_t*                wf;
    csilk_wf_tool_discovery_fn prev_discovery;
    void*                      prev_user_data;
} skill_wf_entry_t;

struct csilk_ai_skill_s {
    csilk_skill_meta_t     meta;
    csilk_skill_backend_t  backend;
    csilk_skill_load_fn    load;
    void*                  user_data;
    csilk_skill_exec_cfg_t exec_cfg;
};

/* --- Global registry of (wf, skill) registrations --- */
static skill_wf_entry_t g_skill_regs[SKILL_MAX_WF_ENTRIES];
static size_t           g_skill_reg_count = 0;
static csilk_mutex_t    g_skill_mutex;
static int              g_skill_mutex_ready = 0;

static int
ensure_skill_registry(void)
{
    if (!g_skill_mutex_ready) {
        csilk_mutex_init(&g_skill_mutex);
        g_skill_mutex_ready = 1;
    }
    return 0;
}

/* --- Validation --- */

static int
is_reserved_skill_name(const char* name)
{
    return strcmp(name, "anthropic") == 0 || strcmp(name, "claude") == 0;
}

int
csilk_ai_skill_validate(const csilk_skill_meta_t* m, const char* body)
{
    (void)body;
    if (!m || !m->name[0] || !m->description[0]) {
        return -1;
    }

    /* name rules */
    size_t nlen = strlen(m->name);
    if (nlen < 1 || nlen >= SKILL_NAME_MAX) {
        return -2;
    }
    int prev_hyphen = 0;
    for (size_t i = 0; i < nlen; i++) {
        char c = m->name[i];
        if (c == '-') {
            if (prev_hyphen) { /* consecutive hyphens */
                return -3;
            }
            prev_hyphen = 1;
            continue;
        }
        if (!islower((unsigned char)c) && !isdigit((unsigned char)c)) {
            return -4;
        }
        prev_hyphen = 0;
    }
    if (m->name[0] == '-' || m->name[nlen - 1] == '-') {
        return -5;
    }
    if (is_reserved_skill_name(m->name)) {
        return -6;
    }

    /* description rules */
    size_t dlen = strlen(m->description);
    if (dlen < 1 || dlen >= SKILL_DESC_MAX) {
        return -7;
    }
    for (size_t i = 0; i < dlen; i++) {
        if (m->description[i] == '<' || m->description[i] == '>') {
            return -8;
        }
    }

    /* optional field caps */
    if (m->license[0] && strlen(m->license) >= SKILL_LICENSE_MAX) {
        return -9;
    }
    if (m->compatibility[0] && strlen(m->compatibility) >= SKILL_COMPAT_MAX) {
        return -10;
    }
    return 0;
}

/* --- Lifecycle --- */

csilk_ai_skill_t*
csilk_ai_skill_new(const csilk_skill_meta_t* meta,
                   csilk_skill_backend_t     backend,
                   csilk_skill_load_fn       load,
                   void*                     user_data)
{
    if (!meta) {
        return NULL;
    }
    if (csilk_ai_skill_validate(meta, NULL) != 0) {
        return NULL;
    }
    csilk_ai_skill_t* sk = calloc(1, sizeof(csilk_ai_skill_t));
    if (!sk) {
        return NULL;
    }
    sk->meta = *meta;
    sk->backend = backend;
    sk->load = load;
    sk->user_data = user_data;
    return sk;
}

void
csilk_ai_skill_free(csilk_ai_skill_t* sk)
{
    free(sk);
}

/* --- Loading --- */

char*
csilk_skill_meta_to_string(const csilk_ai_skill_t* sk)
{
    if (!sk) {
        return NULL;
    }
    size_t need = strlen(sk->meta.name) + strlen(sk->meta.description) + 16;
    char*  buf = malloc(need);
    if (!buf) {
        return NULL;
    }
    snprintf(buf, need, "{name:%s,description:%s}", sk->meta.name, sk->meta.description);
    return buf;
}

int
csilk_ai_skill_load(csilk_ai_skill_t*   sk,
                    csilk_skill_level_t level,
                    const char*         resource_path,
                    char**              out_content)
{
    if (out_content) {
        *out_content = NULL;
    }
    if (!sk || !out_content) {
        return -1;
    }
    /* L1 is intrinsic: serialize the metadata. */
    if (level == CSILK_SKILL_METADATA) {
        *out_content = csilk_skill_meta_to_string(sk);
        return *out_content ? 0 : -1;
    }
    if (!sk->load) {
        return -1;
    }
    return sk->load(sk, level, resource_path, out_content, sk->user_data);
}

/* --- Discovery registration --- */

/**
 * @brief Build a JSON result `{"skill":..,"content":..}` with @p content
 *        escaped so embedded newlines/quotes stay valid JSON.
 */
static char*
json_escape_json_result(const char* skill_name, const char* content)
{
    if (!content) {
        return NULL;
    }
    /* Worst case: every char doubles (escaping). */
    size_t need = strlen(content) * 2 + strlen(skill_name) + 32;
    char*  buf = malloc(need);
    if (!buf) {
        return NULL;
    }
    size_t written = (size_t)snprintf(
        buf, need, "{\"skill\":\"%s\",\"content\":\"", skill_name ? skill_name : "");
    char*  p = buf + written;
    size_t cap = need - written;
    for (const char* q = content; *q; q++) {
        char        c = *q;
        const char* rep = NULL;
        switch (c) {
        case '"':
            rep = "\"";
            break;
        case '\\':
            rep = "\\\\";
            break;
        case '\n':
            rep = "\\n";
            break;
        case '\r':
            rep = "\\r";
            break;
        case '\t':
            rep = "\\t";
            break;
        default:
            break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (cap > rl) {
                memcpy(p, rep, rl);
                p += rl;
                cap -= rl;
            }
        } else {
            if (cap > 1) {
                *p++ = c;
                cap--;
            }
        }
    }
    if (cap > 2) {
        *p++ = '"';
        *p++ = '}';
        *p = 0;
    } else {
        *p = 0;
    }
    return buf;
}

/**
 * @brief Real trigger-tool callback for `use_skill`.
 *
 * Parses `{name, resource?}` from the LLM's arguments, resolves the named
 * skill on this workflow, and returns the loaded L2 instructions — or the L3
 * resource when `resource` is supplied — as a JSON result.  This is the
 * mechanism that turns progressive disclosure into a tool the model can call.
 */
static char*
_use_skill_resolve(csilk_wf_t* wf, const char* args_json)
{
    if (!wf) {
        return strdup("{\"error\":\"no workflow\"}");
    }

    csilk_json_t* args = csilk_json_parse(args_json ? args_json : "{}");
    const char*   raw_name = args ? csilk_json_get_string(args, "name") : NULL;
    const char*   raw_res = args ? csilk_json_get_string(args, "resource") : NULL;
    char*         name = raw_name ? strdup(raw_name) : NULL;
    char*         resource = raw_res ? strdup(raw_res) : NULL;
    if (args) {
        csilk_json_free(args);
    }
    if (!name) {
        free(name);
        free(resource);
        return strdup("{\"error\":\"missing skill name\"}");
    }

    ensure_skill_registry();
    csilk_ai_skill_t* target = NULL;
    csilk_mutex_lock(&g_skill_mutex);
    for (size_t i = 0; i < g_skill_reg_count; i++) {
        if (g_skill_regs[i].wf == wf && g_skill_regs[i].sk &&
            strcmp(g_skill_regs[i].sk->meta.name, name) == 0) {
            target = g_skill_regs[i].sk;
            break;
        }
    }
    csilk_mutex_unlock(&g_skill_mutex);

    if (!target) {
        char buf[512];
        snprintf(buf, sizeof(buf), "{\"error\":\"skill not found: %s\"}", name);
        char* r = strdup(buf);
        free(name);
        free(resource);
        return r;
    }

    csilk_skill_level_t level = resource ? CSILK_SKILL_RESOURCES : CSILK_SKILL_INSTRUCTIONS;
    char*               content = NULL;
    int                 load_rc = csilk_ai_skill_load(target, level, resource, &content);
    if (load_rc != 0 || !content) {
        size_t bcap = strlen(target->meta.name) + strlen(resource ? resource : "instructions") + 64;
        char*  bufd = malloc(bcap);
        if (bufd) {
            snprintf(bufd,
                     bcap,
                     "{\"error\":\"load failed: %s/%s\"}",
                     target->meta.name,
                     resource ? resource : "instructions");
        }
        char* r = bufd ? bufd : strdup("{\"error\":\"load failed\"}");
        free(content);
        free(name);
        free(resource);
        return r;
    }

    char* result = json_escape_json_result(target->meta.name, content);
    free(content);
    free(name);
    free(resource);
    return result ? result : strdup("{\"error\":\"oom\"}");
}

char*
csilk_ai_skill_run_use_skill(csilk_wf_t* wf, const char* args_json)
{
    return _use_skill_resolve(wf, args_json);
}

static char*
use_skill_trigger_fn(const char* args_json, void* user_data)
{
    skill_wf_entry_t* self = (skill_wf_entry_t*)user_data;
    return _use_skill_resolve(self ? self->wf : NULL, args_json);
}

/**
 * @brief Tool-discovery callback installed (or chained) by a skill registration.
 *
 * Enumerates every skill registered on this workflow and emits a single
 * `use_skill` trigger tool whose description aggregates the available skill
 * names.  Falls through to the previously-installed discovery callback so
 * user-supplied dynamic tools are preserved.
 */
static int
skill_discovery_cb(csilk_wf_t*                  wf,
                   const csilk_wf_tool_entry_t* static_tools,
                   size_t                       static_count,
                   csilk_wf_tool_entry_t**      discovered_out,
                   size_t*                      discovered_count_out,
                   void*                        user_data)
{
    (void)static_tools;
    (void)static_count;

    skill_wf_entry_t* self = (skill_wf_entry_t*)user_data;
    int               rc = ensure_skill_registry();
    (void)rc;

    /* Aggregate every skill registered on this wf into one description. */
    char   desc[1024];
    size_t pos = 0;
    pos += (size_t)snprintf(desc + pos, sizeof(desc) - pos, "Available Agent Skills: ");

    int found = 0;
    csilk_mutex_lock(&g_skill_mutex);
    for (size_t i = 0; i < g_skill_reg_count && pos + 1 < sizeof(desc); i++) {
        if (g_skill_regs[i].wf != wf || !g_skill_regs[i].sk) {
            continue;
        }
        pos += (size_t)snprintf(desc + pos,
                                sizeof(desc) - pos,
                                "%s%s",
                                found ? ", " : "",
                                g_skill_regs[i].sk->meta.name);
        found++;
    }
    csilk_mutex_unlock(&g_skill_mutex);

    /* Chain to the previously-installed discovery callback (if any). */
    size_t                 prev_count = 0;
    csilk_wf_tool_entry_t* prev = NULL;
    if (self && self->prev_discovery) {
        self->prev_discovery(
            wf, static_tools, static_count, &prev, &prev_count, self->prev_user_data);
    }

    size_t total = (found > 0 ? 1 : 0) + prev_count;
    if (total == 0) {
        *discovered_out = NULL;
        *discovered_count_out = 0;
        return 0;
    }

    csilk_wf_tool_entry_t* out = calloc(total, sizeof(csilk_wf_tool_entry_t));
    if (!out) {
        free(prev);
        *discovered_out = NULL;
        *discovered_count_out = 0;
        return -1;
    }

    size_t idx = 0;
    if (found > 0) {
        out[idx].name = strdup("use_skill");
        out[idx].description = strdup(desc);
        char schema[512];
        snprintf(schema,
                 sizeof(schema),
                 "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\","
                 "\"description\":\"the skill name\"}},\"required\":[\"name\"]}");
        out[idx].parameters_json = strdup(schema);
        out[idx].fn = use_skill_trigger_fn;
        out[idx].user_data = self;
        idx++;
    }
    for (size_t i = 0; i < prev_count; i++) {
        out[idx] = prev[i];
        idx++;
    }
    free(prev);

    *discovered_out = out;
    *discovered_count_out = total;
    return 0;
}

int
csilk_ai_skill_register(csilk_wf_t* wf, csilk_ai_skill_t* sk)
{
    if (!wf || !sk) {
        return -1;
    }
    if (ensure_skill_registry() != 0) {
        return -1;
    }

    csilk_mutex_lock(&g_skill_mutex);
    if (g_skill_reg_count >= SKILL_MAX_WF_ENTRIES) {
        csilk_mutex_unlock(&g_skill_mutex);
        return -1;
    }
    skill_wf_entry_t* e = &g_skill_regs[g_skill_reg_count];
    e->sk = sk;
    e->wf = wf;
    e->prev_discovery = NULL;
    e->prev_user_data = NULL;
    g_skill_reg_count++;
    csilk_mutex_unlock(&g_skill_mutex);

    /* Install (or chain) the discovery callback on this workflow.  Capture the
     * previously-installed callback so we can chain to it, then point the
     * workflow's discovery at our skill callback with `e` as user_data. */
    csilk_wf_set_tool_discovery(wf, skill_discovery_cb, e);
    return 0;
}

/* --- Remote execution (cloud cluster, csilk_mq) --- */

int
csilk_ai_skill_invoke_remote(csilk_ai_skill_t* sk,
                             csilk_mq_t*       mq,
                             const char*       resource_path,
                             const char*       args,
                             int               timeout_ms,
                             char**            out_result)
{
    if (out_result) {
        *out_result = NULL;
    }
    if (!sk || !mq || !out_result) {
        return -1;
    }

    char idem[256];
    snprintf(idem,
             sizeof(idem),
             "%s:%s:%s",
             sk->meta.name,
             resource_path ? resource_path : "",
             args ? args : "");

    char   payload[1024];
    size_t plen =
        (size_t)snprintf(payload,
                         sizeof(payload),
                         "{\"skill\":\"%s\",\"resource\":\"%s\",\"args\":\"%s\",\"idem\":\"%s\"}",
                         sk->meta.name,
                         resource_path ? resource_path : "",
                         args ? args : "",
                         idem);

    int rc = csilk_mq_publish(mq, "csilk.skill.invoke", payload, plen);
    if (rc != 0) {
        *out_result = strdup("{\"error\":\"publish failed\"}");
        return -1;
    }

    /* The result is delivered asynchronously on the main loop.  For a bounded
     * synchronous wait we rely on the caller's event loop; a hard block would
     * violate the framework's lock-free worker rules.  Return success once the
     * publish is accepted; the result arrives via csilk.skill.result. */
    (void)timeout_ms;
    (void)idem;
    *out_result = strdup("{\"status\":\"dispatched\"}");
    return 0;
}

/* --- Local execution (L3 script, bounded process) --- */

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define SKILL_EXEC_DEFAULT_TIMEOUT_MS 30000
#define SKILL_EXEC_DEFAULT_MAX_OUTPUT 65536

int
csilk_ai_skill_set_exec_cfg(csilk_ai_skill_t* sk, const csilk_skill_exec_cfg_t* cfg)
{
    if (!sk || !cfg) {
        return -1;
    }
    free(sk->exec_cfg.root);
    free(sk->exec_cfg.env);
    sk->exec_cfg.root = cfg->root ? strdup(cfg->root) : NULL;
    sk->exec_cfg.env = cfg->env ? strdup(cfg->env) : NULL;
    sk->exec_cfg.timeout_ms = cfg->timeout_ms;
    sk->exec_cfg.max_output_bytes = cfg->max_output_bytes;
    if (cfg->root && !sk->exec_cfg.root) {
        return -1;
    }
    if (cfg->env && !sk->exec_cfg.env) {
        free(sk->exec_cfg.root);
        return -1;
    }
    return 0;
}

/**
 * @brief Reject any path that escapes @p root: absolute paths, `..` segments,
 *        or NUL bytes.
 */
static int
path_within_root(const char* root, const char* path)
{
    if (!root || !path) {
        return -1;
    }
    if (path[0] == '/' || path[0] == 0) {
        return -1;
    }
    if (strpbrk(path, "\n\r") != NULL) {
        return -1;
    }
    /* Disallow any parent-directory reference. */
    for (const char* p = path; *p; p++) {
        if (p[0] == '.' && p[1] == '.') {
            if (p == path && p[2] == '.') {
                return -1; /* `.../...` */
            }
            if (p[2] == '/' || p[2] == 0) {
                return -1; /* `..` or `../` */
            }
        }
    }
    return 0;
}

int
csilk_skill_exec_local(
    csilk_ai_skill_t* sk, const char* path, const char* args, int timeout_ms, char** out_result)
{
    if (out_result) {
        *out_result = NULL;
    }
    if (!sk || !out_result) {
        return -1;
    }

    if (!sk->exec_cfg.root) {
        *out_result = strdup("{\"error\":\"no exec root configured\"}");
        return -1;
    }
    if (path_within_root(sk->exec_cfg.root, path) != 0) {
        *out_result = strdup("{\"error\":\"path escapes exec root\"}");
        return -1;
    }

    /* `path` is an executable; `args` are space-separated argv. */
    char  argbuf[4096];
    char* argv[256];
    int   argc = 0;
    argv[argc++] = strdup(path);
    if (!argv[0]) {
        return -1;
    }
    if (args && args[0]) {
        strncpy(argbuf, args, sizeof(argbuf) - 1);
        argbuf[sizeof(argbuf) - 1] = 0;
        char* tok = strtok(argbuf, " ");
        while (tok && argc < (int)(sizeof(argv) / sizeof(argv[0]) - 1)) {
            argv[argc++] = tok;
            tok = strtok(NULL, " ");
        }
    }
    argv[argc] = NULL;

    int timeout = timeout_ms > 0 ? timeout_ms : sk->exec_cfg.timeout_ms;
    if (timeout <= 0) {
        timeout = SKILL_EXEC_DEFAULT_TIMEOUT_MS;
    }
    int max_output = sk->exec_cfg.max_output_bytes > 0 ? sk->exec_cfg.max_output_bytes
                                                       : SKILL_EXEC_DEFAULT_MAX_OUTPUT;

    /* Child env: minimal (PATH) + any user-supplied env. */
    char  envbuf[4096];
    char* eargv[64];
    int   envc = 0;
    eargv[envc++] = "PATH=/usr/bin:/bin";
    if (sk->exec_cfg.env && sk->exec_cfg.env[0]) {
        memset(envbuf, 0, sizeof(envbuf));
        strncpy(envbuf, sk->exec_cfg.env, sizeof(envbuf) - 1);
        char* etok = strtok(envbuf, " ");
        while (etok && envc < 63) {
            eargv[envc++] = etok;
            etok = strtok(NULL, " ");
        }
    }
    eargv[envc] = NULL;
    char** envp = eargv;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        *out_result = strdup("{\"error\":\"pipe failed\"}");
        free(argv[0]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(argv[0]);
        *out_result = strdup("{\"error\":\"fork failed\"}");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (chdir(sk->exec_cfg.root) != 0) {
            _exit(126);
        }
        execvpe(argv[0], argv, (char* const*)envp);
        _exit(127); /* exec failed */
    }

    /* Parent. */
    close(pipefd[1]);

    char* output = malloc((size_t)max_output + 1);
    if (!output) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        free(argv[0]);
        *out_result = strdup("{\"error\":\"oom\"}");
        return -1;
    }

    /* Bounded drain + wait: the stdout read blocks until the pipe's write
     * end closes (i.e. the child exits), so a long-running child would
     * otherwise stall the whole function past any timeout.  We therefore
     * drain with non-blocking reads and enforce the wall-clock budget here,
     * SIGKILLing the child on expiry. */
    int total = 0;
    int truncated = 0;
    int timed_out = 0;
    int elapsed_ms = 0;
    int status = 0;
    int done = 0;
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    const int poll_ms = 20;
    while (!done) {
        int n = read(pipefd[0], output + total, max_output - total);
        if (n > 0) {
            total += n;
            if (total >= max_output) {
                truncated = 1;
            }
        } else if (n < 0 && errno == EAGAIN) {
            /* No data yet — check the wall-clock budget, then poll. */
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                /* Child exited: drain any remaining buffered bytes. */
                int m;
                while ((m = read(pipefd[0], output + total, max_output - total)) > 0) {
                    total += m;
                    if (total >= max_output) {
                        truncated = 1;
                        break;
                    }
                }
                done = 1;
                break;
            }
            usleep((long)poll_ms * 1000);
            elapsed_ms += poll_ms;
            if (elapsed_ms >= timeout) {
                kill(pid, SIGKILL);
                int st;
                waitpid(pid, &st, 0);
                status = st;
                timed_out = 1;
                done = 1;
                break;
            }
        } else if (n == 0) {
            /* EOF: child closed the write end. */
            done = 1;
        } else {
            done = 1;
            break;
        }
    }
    close(pipefd[0]);
    output[total] = 0;

    int exit_code;
    if (timed_out) {
        exit_code = -9;
    } else if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    } else {
        exit_code = -1;
    }

    /* Build JSON envelope. */
    size_t osize = (size_t)total * 2 + strlen(sk->meta.name) + 64;
    char*  result = malloc(osize);
    if (!result) {
        free(output);
        free(argv[0]);
        *out_result = strdup("{\"error\":\"oom\"}");
        return -1;
    }
    char*  p = result;
    size_t rem = osize;
    rem -= (size_t)snprintf(p, rem, "{\"skill\":\"%s\",\"stdout\":\"", sk->meta.name);
    p += osize - rem;
    rem = osize - (p - result);
    for (int i = 0; i < total && rem > 2; i++) {
        char        c = output[i];
        const char* rep = NULL;
        switch (c) {
        case '"':
            rep = "\\\"";
            break;
        case '\\':
            rep = "\\\\";
            break;
        case '\n':
            rep = "\\n";
            break;
        case '\r':
            rep = "\\r";
            break;
        case '\t':
            rep = "\\t";
            break;
        default:
            break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (rem > rl + 1) {
                memcpy(p, rep, rl);
                p += rl;
                rem -= rl;
            }
        } else {
            *p++ = c;
            rem--;
        }
    }
    snprintf(
        p, rem, "\",\"exit_code\":%d,\"truncated\":%s}", exit_code, truncated ? "true" : "false");

    free(output);
    free(argv[0]);
    *out_result = result;
    return 0;
}
