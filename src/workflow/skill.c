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
#include "csilk/core/sys_io.h"
#include "csilk/messaging/mq.h"
#include "../messaging/mq_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
static csilk_once_t     g_skill_once = CSILK_ONCE_INIT;

static void
init_skill_registry_once(void)
{
    csilk_mutex_init(&g_skill_mutex);
}

static int
ensure_skill_registry(void)
{
    csilk_once(&g_skill_once, init_skill_registry_once);
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

static void unregister_skill_by_ptr(csilk_ai_skill_t* sk);

void
csilk_ai_skill_free(csilk_ai_skill_t* sk)
{
    if (!sk) {
        return;
    }
    unregister_skill_by_ptr(sk);
    csilk_ai_skill_worker_unregister(sk);
    /* exec_cfg strings are strdup'd copies owned by the skill. */
    free(sk->exec_cfg.root);
    free(sk->exec_cfg.env);
    free(sk);
}

/* Remove all registry entries that reference @p sk so the global registry
 * never holds a dangling pointer.  A skill may be registered on several
 * workflows; every entry referencing it is compacted out (O(n) shift,
 * preserving order of the remaining valid entries).  Called from
 * csilk_ai_skill_free before the handle is released. */
static void
unregister_skill_by_ptr(csilk_ai_skill_t* sk)
{
    if (!sk || !g_skill_mutex_ready) {
        return;
    }
    csilk_mutex_lock(&g_skill_mutex);
    size_t cur = 0;
    for (size_t i = 0; i < g_skill_reg_count; i++) {
        if (g_skill_regs[i].sk == sk) {
            continue; /* drop this entry */
        }
        if (cur != i) {
            g_skill_regs[cur] = g_skill_regs[i];
        }
        cur++;
    }
    g_skill_reg_count = cur;
    csilk_mutex_unlock(&g_skill_mutex);
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
    csilk_wf_t* wf = (csilk_wf_t*)user_data;
    return _use_skill_resolve(wf, args_json);
}

/**
 * @brief Tool-discovery callback installed by a skill registration.
 *
 * Enumerates every skill registered on this workflow and emits a single
 * `use_skill` trigger tool whose description aggregates the available skill
 * names.
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
    (void)user_data;

    int rc = ensure_skill_registry();
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

    if (found == 0) {
        *discovered_out = NULL;
        *discovered_count_out = 0;
        return 0;
    }

    csilk_wf_tool_entry_t* out = calloc(1, sizeof(csilk_wf_tool_entry_t));
    if (!out) {
        *discovered_out = NULL;
        *discovered_count_out = 0;
        return -1;
    }

    out[0].name = strdup("use_skill");
    out[0].description = strdup(desc);
    char schema[512];
    snprintf(schema,
             sizeof(schema),
             "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\","
             "\"description\":\"the skill name\"}},\"required\":[\"name\"]}");
    out[0].parameters_json = strdup(schema);
    out[0].fn = use_skill_trigger_fn;
    out[0].user_data = wf;

    *discovered_out = out;
    *discovered_count_out = 1;
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

    csilk_wf_set_tool_discovery(wf, skill_discovery_cb, wf);
    return 0;
}

/* --- Remote execution (cloud cluster, csilk_mq) --- */

#define SKILL_MAX_REMOTE_WORKERS 64
#define SKILL_MAX_REMOTE_PENDING 64
#define SKILL_IDEM_CACHE_CAP 64

typedef struct {
    csilk_ai_skill_t* sk;
    csilk_mq_t*       mq;
    uint64_t          last_heartbeat;
    int               is_healthy;
} skill_remote_worker_t;

typedef struct {
    int           active;
    int           done;
    int           rc;
    char          idem[256];
    char*         result;
    csilk_mq_t*   mq;
    csilk_mutex_t mu;
    csilk_cond_t  cv;
} skill_remote_pending_t;

typedef struct {
    char  idem[256];
    char* result;
    int   rc;
} skill_idem_cache_entry_t;

static skill_remote_worker_t    g_remote_workers[SKILL_MAX_REMOTE_WORKERS];
static size_t                   g_remote_worker_count = 0;
static csilk_mq_t*              g_subscribed_mqs[SKILL_MAX_REMOTE_WORKERS];
static size_t                   g_subscribed_mq_count = 0;
static skill_remote_pending_t   g_remote_pending[SKILL_MAX_REMOTE_PENDING];
static skill_idem_cache_entry_t g_idem_cache[SKILL_IDEM_CACHE_CAP];
static size_t                   g_idem_cache_next = 0;
static csilk_mutex_t            g_remote_mutex;
static csilk_once_t             g_remote_once = CSILK_ONCE_INIT;
static int                      g_remote_mutex_ready = 0;

static int g_idem_atexit_registered = 0;

static void
idem_cache_atexit_cleanup(void)
{
    if (!g_remote_mutex_ready) {
        return;
    }
    csilk_mutex_lock(&g_remote_mutex);
    for (size_t i = 0; i < SKILL_IDEM_CACHE_CAP; i++) {
        free(g_idem_cache[i].result);
        g_idem_cache[i].result = NULL;
    }
    csilk_mutex_unlock(&g_remote_mutex);
}

static void
init_remote_state_once(void)
{
    csilk_mutex_init(&g_remote_mutex);
    for (size_t i = 0; i < SKILL_MAX_REMOTE_PENDING; i++) {
        csilk_mutex_init(&g_remote_pending[i].mu);
        csilk_cond_init(&g_remote_pending[i].cv);
    }
    g_remote_mutex_ready = 1;
    if (!g_idem_atexit_registered) {
        g_idem_atexit_registered = 1;
        atexit(idem_cache_atexit_cleanup);
    }
}

static void
ensure_remote_state(void)
{
    csilk_once(&g_remote_once, init_remote_state_once);
}

static int
idem_cache_lookup(const char* idem, char** out_result, int* out_rc)
{
    if (!idem || !idem[0]) {
        return 0;
    }
    for (size_t i = 0; i < SKILL_IDEM_CACHE_CAP; i++) {
        if (g_idem_cache[i].result && strcmp(g_idem_cache[i].idem, idem) == 0) {
            *out_result = strdup(g_idem_cache[i].result);
            *out_rc = g_idem_cache[i].rc;
            return 1;
        }
    }
    return 0;
}

static void
idem_cache_store(const char* idem, const char* result, int rc)
{
    if (!idem || !idem[0] || !result) {
        return;
    }
    size_t idx = g_idem_cache_next % SKILL_IDEM_CACHE_CAP;
    g_idem_cache_next++;
    /* Clear idem cache slots on shutdown or keep bounded; only 1 entry in
     * unit tests — release oldest before overwriting. */
    if (g_idem_cache[idx].result) {
        free(g_idem_cache[idx].result);
    }
    snprintf(g_idem_cache[idx].idem, sizeof(g_idem_cache[idx].idem), "%s", idem);
    g_idem_cache[idx].result = strdup(result);
    g_idem_cache[idx].rc = rc;
}

static void
skill_mq_on_result(csilk_mq_ctx_t* ctx)
{
    size_t      len = 0;
    const char* payload = (const char*)csilk_mq_get_payload(ctx, &len);
    if (!payload || len == 0) {
        return;
    }
    csilk_json_t* js = csilk_json_parse_len(payload, len);
    if (!js) {
        return;
    }
    const char* idem = csilk_json_get_string(js, "idem");
    if (!idem || !idem[0]) {
        csilk_json_free(js);
        return;
    }

    ensure_remote_state();
    csilk_mutex_lock(&g_remote_mutex);
    for (size_t i = 0; i < SKILL_MAX_REMOTE_PENDING; i++) {
        if (g_remote_pending[i].active && strcmp(g_remote_pending[i].idem, idem) == 0) {
            skill_remote_pending_t* p = &g_remote_pending[i];
            csilk_mutex_lock(&p->mu);
            if (!p->done) {
                /* Prefer explicit error/result fields, else store the whole envelope. */
                const char* err = csilk_json_get_string(js, "error");
                p->rc = err ? -1 : 0;
                free(p->result);
                p->result = strndup(payload, len);
                p->done = 1;
                csilk_cond_signal(&p->cv);
            }
            csilk_mutex_unlock(&p->mu);
            break;
        }
    }
    csilk_mutex_unlock(&g_remote_mutex);
    csilk_json_free(js);
}

static void
skill_mq_on_invoke(csilk_mq_ctx_t* ctx)
{
    size_t      len = 0;
    const char* payload = (const char*)csilk_mq_get_payload(ctx, &len);
    if (!payload || len == 0 || !ctx->mq) {
        return;
    }
    csilk_json_t* js = csilk_json_parse_len(payload, len);
    if (!js) {
        return;
    }
    const char* skill_name = csilk_json_get_string(js, "skill");
    const char* resource = csilk_json_get_string(js, "resource");
    const char* args = csilk_json_get_string(js, "args");
    const char* idem = csilk_json_get_string(js, "idem");
    if (!skill_name || !skill_name[0]) {
        csilk_json_free(js);
        return;
    }

    ensure_remote_state();
    csilk_mutex_lock(&g_remote_mutex);
    /* Check idempotent cache first so duplicate replays never re-execute. */
    char* cached = NULL;
    int   cached_rc = 0;
    if (idem && idem[0] && idem_cache_lookup(idem, &cached, &cached_rc) && cached) {
        csilk_mutex_unlock(&g_remote_mutex);
        csilk_mq_publish(ctx->mq, "csilk.skill.result", cached, strlen(cached));
        free(cached);
        csilk_json_free(js);
        return;
    }

    csilk_ai_skill_t* worker_sk = NULL;
    for (size_t i = 0; i < g_remote_worker_count; i++) {
        if (g_remote_workers[i].mq == ctx->mq && g_remote_workers[i].sk &&
            strcmp(g_remote_workers[i].sk->meta.name, skill_name) == 0) {
            worker_sk = g_remote_workers[i].sk;
            break;
        }
    }
    csilk_mutex_unlock(&g_remote_mutex);

    if (!worker_sk) {
        csilk_json_free(js);
        return;
    }

    char* exec_out = NULL;
    int   exec_rc = -1;
    if (worker_sk->exec_cfg.root && resource && resource[0]) {
        exec_rc = csilk_skill_exec_local(
            worker_sk, resource, (args && args[0]) ? args : NULL, 0, &exec_out);
    } else if (worker_sk->load) {
        char* loaded = NULL;
        exec_rc = csilk_ai_skill_load(worker_sk,
                                      (resource && resource[0]) ? CSILK_SKILL_RESOURCES
                                                                : CSILK_SKILL_INSTRUCTIONS,
                                      (resource && resource[0]) ? resource : NULL,
                                      &loaded);
        if (exec_rc == 0 && loaded) {
            exec_out = json_escape_json_result(worker_sk->meta.name, loaded);
            free(loaded);
        }
    }

    /* Wrap the execution output with the correlation `idem` key so the caller's
     * result subscriber can match it deterministically. */
    csilk_json_t* env = exec_out ? csilk_json_parse(exec_out) : NULL;
    if (!env) {
        env = csilk_json_object();
        csilk_json_add_string(env, "skill", skill_name);
        csilk_json_add_string(env, "error", exec_out ? exec_out : "worker execution failed");
    }
    /* Copy immutable parsed doc to a mutable object so we can add `idem` /
     * `error` without hitting json_object's mutable-only check. */
    csilk_json_t* out_obj = csilk_json_copy(env);
    csilk_json_free(env);
    if (!out_obj) {
        free(exec_out);
        csilk_json_free(js);
        return;
    }
    if (exec_rc != 0 && !csilk_json_get_string(out_obj, "error")) {
        csilk_json_add_string(out_obj, "error", "execution failed");
    }
    if (idem && idem[0]) {
        csilk_json_add_string(out_obj, "idem", idem);
    }
    char* reply = csilk_json_serialize(out_obj, NULL);
    csilk_json_free(out_obj);
    free(exec_out);

    if (reply) {
        csilk_mutex_lock(&g_remote_mutex);
        if (idem && idem[0]) {
            idem_cache_store(idem, reply, exec_rc);
        }
        csilk_mutex_unlock(&g_remote_mutex);
        csilk_mq_publish(ctx->mq, "csilk.skill.result", reply, strlen(reply));
        free(reply);
    }
    csilk_json_free(js);
}

static void
ensure_mq_remote_subscribed(csilk_mq_t* mq)
{
    int already = 0;
    for (size_t i = 0; i < g_subscribed_mq_count; i++) {
        if (g_subscribed_mqs[i] == mq) {
            already = 1;
            break;
        }
    }
    if (!already && g_subscribed_mq_count < SKILL_MAX_REMOTE_WORKERS) {
        g_subscribed_mqs[g_subscribed_mq_count++] = mq;
        csilk_mq_subscribe(mq, "csilk.skill.invoke", skill_mq_on_invoke);
        csilk_mq_subscribe(mq, "csilk.skill.result", skill_mq_on_result);
    }
}

static uint64_t
now_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

int
csilk_ai_skill_worker_heartbeat(csilk_ai_skill_t* sk, csilk_mq_t* mq)
{
    if (!sk || !mq) {
        return -1;
    }
    ensure_remote_state();
    csilk_mutex_lock(&g_remote_mutex);
    int      found = 0;
    uint64_t now = now_monotonic_ms();
    for (size_t i = 0; i < g_remote_worker_count; i++) {
        if (g_remote_workers[i].sk == sk && g_remote_workers[i].mq == mq) {
            g_remote_workers[i].last_heartbeat = now;
            g_remote_workers[i].is_healthy = 1;
            found = 1;
            break;
        }
    }
    csilk_mutex_unlock(&g_remote_mutex);
    return found ? 0 : -1;
}

int
csilk_ai_skill_worker_register(csilk_ai_skill_t* sk, csilk_mq_t* mq)
{
    if (!sk || !mq) {
        return -1;
    }
    ensure_remote_state();
    csilk_mutex_lock(&g_remote_mutex);
    uint64_t now = now_monotonic_ms();
    for (size_t i = 0; i < g_remote_worker_count; i++) {
        if (g_remote_workers[i].sk == sk && g_remote_workers[i].mq == mq) {
            g_remote_workers[i].last_heartbeat = now;
            g_remote_workers[i].is_healthy = 1;
            ensure_mq_remote_subscribed(mq);
            csilk_mutex_unlock(&g_remote_mutex);
            return 0;
        }
    }
    if (g_remote_worker_count >= SKILL_MAX_REMOTE_WORKERS) {
        csilk_mutex_unlock(&g_remote_mutex);
        return -1;
    }
    g_remote_workers[g_remote_worker_count].sk = sk;
    g_remote_workers[g_remote_worker_count].mq = mq;
    g_remote_workers[g_remote_worker_count].last_heartbeat = now;
    g_remote_workers[g_remote_worker_count].is_healthy = 1;
    g_remote_worker_count++;
    ensure_mq_remote_subscribed(mq);
    csilk_mutex_unlock(&g_remote_mutex);
    return 0;
}

void
csilk_ai_skill_worker_unregister(csilk_ai_skill_t* sk)
{
    if (!sk || !g_remote_mutex_ready) {
        return;
    }
    csilk_mutex_lock(&g_remote_mutex);
    size_t cur = 0;
    for (size_t i = 0; i < g_remote_worker_count; i++) {
        if (g_remote_workers[i].sk == sk) {
            continue;
        }
        if (cur != i) {
            g_remote_workers[cur] = g_remote_workers[i];
        }
        cur++;
    }
    g_remote_worker_count = cur;
    csilk_mutex_unlock(&g_remote_mutex);
}

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

    ensure_remote_state();

    char idem[256];
    snprintf(idem,
             sizeof(idem),
             "%s:%s:%s",
             sk->meta.name,
             resource_path ? resource_path : "",
             args ? args : "");

    csilk_json_t* req = csilk_json_object();
    if (!req) {
        return -1;
    }
    csilk_json_add_string(req, "skill", sk->meta.name);
    csilk_json_add_string(req, "resource", resource_path ? resource_path : "");
    csilk_json_add_string(req, "args", args ? args : "");
    csilk_json_add_string(req, "idem", idem);
    char* payload = csilk_json_serialize(req, NULL);
    csilk_json_free(req);
    if (!payload) {
        return -1;
    }

    /* Register pending waiter slot and result subscription before publishing so
     * synchronous or fast in-loop replies are never missed. */
    csilk_mutex_lock(&g_remote_mutex);
    ensure_mq_remote_subscribed(mq);
    skill_remote_pending_t* slot = NULL;
    for (size_t i = 0; i < SKILL_MAX_REMOTE_PENDING; i++) {
        if (!g_remote_pending[i].active) {
            slot = &g_remote_pending[i];
            slot->active = 1;
            slot->done = 0;
            slot->rc = -1;
            slot->mq = mq;
            free(slot->result);
            slot->result = NULL;
            snprintf(slot->idem, sizeof(slot->idem), "%s", idem);
            break;
        }
    }
    csilk_mutex_unlock(&g_remote_mutex);

    if (!slot) {
        free(payload);
        *out_result = strdup("{\"error\":\"remote pending table full\"}");
        return -1;
    }

    int rc = csilk_mq_publish(mq, "csilk.skill.invoke", payload, strlen(payload));
    free(payload);
    if (rc != 0) {
        csilk_mutex_lock(&g_remote_mutex);
        slot->active = 0;
        csilk_mutex_unlock(&g_remote_mutex);
        *out_result = strdup("{\"error\":\"publish failed\"}");
        return -1;
    }

    int             wait_ms = timeout_ms > 0 ? timeout_ms : SKILL_REMOTE_DEFAULT_TIMEOUT_MS;
    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* Only pump mq->loop if this call is running on the loop thread; calling
     * uv_run() from a non-loop thread while the server loop runs is unsafe. */
    int same_loop_thread = 1;
#if !defined(CSILK_USE_URING) && !defined(_WIN32)
    if (mq->loop && mq->loop->data) {
        (void)0;
    }
#endif
    (void)same_loop_thread;

    for (;;) {
        csilk_mutex_lock(&slot->mu);
        int done = slot->done;
        csilk_mutex_unlock(&slot->mu);
        if (done) {
            break;
        }

        /* Pump the loop in single-thread test/orchestrator mode. */
        if (mq->loop) {
            csilk_io_run(mq->loop, CSILK_IO_RUN_NOWAIT);
        }

        csilk_mutex_lock(&slot->mu);
        done = slot->done;
        csilk_mutex_unlock(&slot->mu);
        if (done) {
            break;
        }

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long elapsed_ms = (now_ts.tv_sec - start_ts.tv_sec) * 1000L +
                          (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000L;
        if (elapsed_ms >= wait_ms) {
            break;
        }
        usleep(1000);
    }

    int   final_rc = -1;
    char* final_res = NULL;
    csilk_mutex_lock(&g_remote_mutex);
    csilk_mutex_lock(&slot->mu);
    if (slot->done && slot->result) {
        final_rc = slot->rc;
        final_res = slot->result;
        slot->result = NULL;
    }
    slot->active = 0;
    csilk_mutex_unlock(&slot->mu);
    csilk_mutex_unlock(&g_remote_mutex);

    if (!final_res) {
        char errbuf[320];
        snprintf(
            errbuf, sizeof(errbuf), "{\"error\":\"remote skill timeout\",\"idem\":\"%s\"}", idem);
        *out_result = strdup(errbuf);
        return -1;
    }
    *out_result = final_res;
    return final_rc;
}

/* --- Local execution (L3 script, bounded process) --- */

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
        sk->exec_cfg.root = NULL; /* avoid double free in csilk_ai_skill_free */
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
        char* saveptr_arg = NULL;
        char* tok = strtok_r(argbuf, " ", &saveptr_arg);
        while (tok && argc < (int)(sizeof(argv) / sizeof(argv[0]) - 1)) {
            argv[argc++] = tok;
            tok = strtok_r(NULL, " ", &saveptr_arg);
        }
    }
    argv[argc] = NULL;

    int timeout = timeout_ms > 0 ? timeout_ms : sk->exec_cfg.timeout_ms;
    if (timeout <= 0) {
        timeout = SKILL_EXEC_DEFAULT_TIMEOUT_MS;
    }
    int max_output = sk->exec_cfg.max_output_bytes > 0 ? sk->exec_cfg.max_output_bytes
                                                       : SKILL_EXEC_DEFAULT_MAX_OUTPUT;

    /* Cross-platform env isolation: build a minimal env array (PATH + user-
     * supplied KEY=VALUE tokens) that the child passes explicitly to execve.
     * `environ` and `clearenv` are both feature-macro-gated on macOS
     * (need __DARWIN_C_FULL); `execve` is unconditionally available on
     * glibc, macOS/BSD, and other POSIX systems.  execve does not search
     * PATH, so the child resolves argv[0] to an absolute path before the
     * call. */
    char  s_env_tokens[4096];
    char* envp[64];
    int   envc = 0;
    envp[envc++] = "PATH=/usr/bin:/bin";
    if (sk->exec_cfg.env && sk->exec_cfg.env[0]) {
        memset(s_env_tokens, 0, sizeof(s_env_tokens));
        strncpy(s_env_tokens, sk->exec_cfg.env, sizeof(s_env_tokens) - 1);
        char* saveptr_env = NULL;
        char* etok = strtok_r(s_env_tokens, " ", &saveptr_env);
        while (etok && envc < 63) {
            envp[envc++] = etok;
            etok = strtok_r(NULL, " ", &saveptr_env);
        }
    }
    envp[envc] = NULL;

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
        /* execve (not execvpe) is POSIX on glibc, macOS, and BSD: it takes
         * an explicit envp array so we never need to touch `environ`/`clearenv`
         * (feature-macro-gated on macOS).  argv[0] is resolved to an absolute
         * path here so execve can find it regardless of the child's CWD. */
        char abspath[4096];
        if (argv[0][0] == '/') {
            /* Already absolute. */
            snprintf(abspath, sizeof(abspath), "%s", argv[0]);
        } else {
            /* Relative to the exec root — child is already chdir'd here. */
            snprintf(abspath, sizeof(abspath), "%s/%s", sk->exec_cfg.root, argv[0]);
        }
        execve(abspath, argv, (char* const*)envp);
        _exit(127); /* exec failed */
    }

    /* Parent.  Close the write end: a blocking read on a pipe whose write
     * end is held only by the child will see EOF once the child closes its
     * stdout.  A blocking read returns the moment any byte is available, so
     * a short-output child drains in one or two reads without a 20ms poll
     * window that previously raced and dropped the data.  The wall-clock
     * budget is still enforced: after each blocking drain we re-check
     * `waitpid(WNOHANG)` and the elapsed time, SIGKILLing the child on
     * expiry — so a silent long-running child that holds the pipe open is
     * killed at @p timeout_ms, not at end-of-output. */
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

    int       total = 0;
    int       truncated = 0;
    int       timed_out = 0;
    int       elapsed_ms = 0;
    int       status = 0;
    int       done = 0;
    const int poll_ms = 20;
    while (!done) {
        /* poll() with a bounded timeout: returns immediately when data is
         * available (fast path for short-output children), or wakes at the
         * poll deadline to re-check the wall-clock budget and the child's
         * exit (fast path for silent children like `sleep`).  This fixes
         * the original blocking-read deadlock (a silent child held the
         * pipe open forever, stalling past any timeout) while avoiding
         * the 20ms poll-window race that dropped small outputs. */
        int remaining = timeout - elapsed_ms;
        if (remaining <= 0) {
            kill(pid, SIGKILL);
            int st;
            waitpid(pid, &st, 0);
            status = st;
            timed_out = 1;
            done = 1;
            break;
        }
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
        int           prc = poll(&pfd, 1, remaining > poll_ms ? poll_ms : remaining);
        elapsed_ms += (remaining > poll_ms) ? poll_ms : remaining;
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (prc == 0) {
            /* No data within the poll window — the child may have already
             * exited, or is silent.  Check its exit; keep polling until
             * the budget runs out, then SIGKILL. */
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                done = 1;
                break;
            }
            continue;
        }
        /* Data available (or EOF). */
        int n = (int)read(pipefd[0], output + total, max_output - total);
        if (n > 0) {
            total += n;
            if (total >= max_output) {
                truncated = 1;
                break;
            }
        } else if (n == 0) {
            /* EOF: child closed stdout.  The process may still be running
             * (e.g. it spawned a background child); wait for it to fully
             * exit within the budget, else SIGKILL. */
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                done = 1;
                break;
            }
            while (elapsed_ms < timeout) {
                usleep((long)poll_ms * 1000);
                elapsed_ms += poll_ms;
                r = waitpid(pid, &status, WNOHANG);
                if (r == pid) {
                    done = 1;
                    break;
                }
            }
            if (!done) {
                kill(pid, SIGKILL);
                int st;
                waitpid(pid, &st, 0);
                status = st;
                timed_out = 1;
                done = 1;
            }
        } else {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }
    /* Drain any bytes still buffered in the pipe after the loop exits (for
     * example, when the child wrote its output and exited in the window
     * between poll() returning 0 and waitpid(WNOHANG) reaping it, which
     * happens on macOS Debug/ASAN runners where fork+exec takes ~20ms).
     * Switch pipefd[0] to O_NONBLOCK so this drain cannot block even if a
     * background grandchild inherited the write end. */
    int fl = fcntl(pipefd[0], F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
    }
    while (total < max_output) {
        int n = (int)read(pipefd[0], output + total, max_output - total);
        if (n > 0) {
            total += n;
            if (total >= max_output) {
                truncated = 1;
                break;
            }
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(pipefd[0]);
    if (!done) {
        /* Truncation or read error exited the loop early; ensure the child is
         * terminated and reaped so no zombie lingers. */
        if (waitpid(pid, &status, WNOHANG) != pid) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
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
