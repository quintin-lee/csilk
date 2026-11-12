#pragma once
/**
 * @file skill.h
 * @brief Standard Agent Skill abstraction (Anthropic Agent Skills semantics).
 *
 * A *skill* is a capability bundle: a `SKILL.md` (YAML frontmatter +
 * instructions) plus optional resources/scripts, following the three-level
 * progressive-disclosure model:
 *
 *   - **L1 metadata** (name + description) — always loaded into the agent's
 *     system prompt so the model knows *when* to use a skill.
 *   - **L2 instructions** (the `SKILL.md` body) — loaded only when the skill
 *     is triggered.
 *   - **L3 resources** (bundled files / scripts) — loaded/executed on demand.
 *
 * `csilk` models a skill as a *discoverable trigger tool* injected through the
 * existing workflow tool-discovery hook (`csilk_wf_set_tool_discovery`).  The
 * trigger tool (`use_skill`) carries the L1 metadata; invoking it loads L2 and,
 * if requested, L3.  On a cloud cluster a skill's L3 execution (or an
 * expensive tool call) is offloaded to a worker node over `csilk_mq`
 * (`csilk.skill.invoke` / `csilk.skill.result`) with idempotent replay and
 * timeout-based failover.
 *
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stddef.h>

#include "csilk/app/workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The three progressive-disclosure levels of a skill.
 */
typedef enum {
    CSILK_SKILL_METADATA,     /**< L1: name + description (always in system prompt). */
    CSILK_SKILL_INSTRUCTIONS, /**< L2: SKILL.md body (loaded on trigger). */
    CSILK_SKILL_RESOURCES     /**< L3: bundled resources/scripts (loaded/executed as needed). */
} csilk_skill_level_t;

/**
 * @brief Where a skill's execution happens.
 */
typedef enum {
    CSILK_SKILL_LOCAL, /**< Execute inside the current process. */
    CSILK_SKILL_REMOTE /**< Offload to a cluster worker via csilk_mq. */
} csilk_skill_backend_t;

/** @brief Opaque handle for a skill. */
typedef struct csilk_ai_skill_s csilk_ai_skill_t;

/**
 * @brief L1 metadata of a skill (parsed from `SKILL.md` frontmatter).
 *
 * Field lengths mirror the Agent Skills specification:
 *   - name:        kebab-case `[a-z0-9-]`, 1–64 chars, no leading/trailing or
 *                  consecutive hyphens, not a reserved word.
 *   - description: 1–1024 chars, must state *what* it does *and* *when*.
 */
typedef struct {
    char name[64];
    char description[1024];
    char license[64];        /**< Optional license name / reference. */
    char compatibility[500]; /**< Optional environment requirements. */
} csilk_skill_meta_t;

/**
 * @brief Callback that loads one level of a skill's content.
 *
 * @param sk            The skill handle.
 * @param level         Which level to load.
 * @param resource_path Relative path for L3 (e.g. "scripts/run.sh"); NULL for L1/L2.
 * @param[out] out_content [out] Heap-allocated content (caller owns); set to NULL on failure.
 * @param user_data     Opaque pointer forwarded to @p load.
 * @return 0 on success, -1 on failure.
 */
typedef int (*csilk_skill_load_fn)(csilk_ai_skill_t*   sk,
                                   csilk_skill_level_t level,
                                   const char*         resource_path,
                                   char**              out_content,
                                   void*               user_data);

/* --- Lifecycle --- */

/**
 * @brief Create a skill instance.
 * @param meta      L1 metadata (validated; caller keeps ownership of the struct).
 * @param backend   LOCAL or REMOTE.
 * @param load      Content loader for L1/L2/L3 (may be NULL → L2/L3 unavailable).
 * @param user_data Opaque pointer forwarded to @p load.
 * @return Skill handle, or NULL on OOM / metadata validation failure.
 */
csilk_ai_skill_t* csilk_ai_skill_new(const csilk_skill_meta_t* meta,
                                     csilk_skill_backend_t     backend,
                                     csilk_skill_load_fn       load,
                                     void*                     user_data);

/**
 * @brief Free a skill handle.
 */
void csilk_ai_skill_free(csilk_ai_skill_t* sk);

/* --- Validation --- */

/**
 * @brief Validate L1 metadata against the Agent Skills specification.
 *
 * Enforces the kebab-case name rules, description length/content, and
 * optional-field length caps.
 *
 * @param m       Metadata to validate (may be partially filled).
 * @param body    The SKILL.md body (may be NULL).  Used only to reject
 *               `<`/`>` XML-tag characters in name/description if present.
 * @return 0 if valid, negative error code otherwise.
 */
int csilk_ai_skill_validate(const csilk_skill_meta_t* m, const char* body);

/* --- Loading --- */

/**
 * @brief Load one level of skill content.
 *
 * L1 returns the serialized metadata; L2 the SKILL.md body; L3 the named
 * resource.  Requires a loader set at creation.
 *
 * @param sk            Skill handle.
 * @param level         Which level to load.
 * @param resource_path L3 relative path (ignored for L1/L2, may be NULL).
 * @param[out] out_content [out] Heap-allocated content or NULL.
 * @return 0 on success, -1 on failure.
 */
int csilk_ai_skill_load(csilk_ai_skill_t*   sk,
                        csilk_skill_level_t level,
                        const char*         resource_path,
                        char**              out_content);

/**
 * @brief Serialize L1 metadata to a compact `{name:..,description:..}` string.
 * @param sk Skill handle.
 * @return Heap-allocated string (caller frees) or NULL.
 */
char* csilk_skill_meta_to_string(const csilk_ai_skill_t* sk);

/* --- Discovery / registration --- */

/**
 * @brief Register a skill with a workflow so its L1 metadata becomes
 *        discoverable by AI nodes.
 *
 * Installs (or chains) a tool-discovery callback on @p wf that injects a
 * single `use_skill` trigger tool whose description aggregates the metadata of
 * every skill registered on that workflow.  Multiple skills may be registered
 * on the same workflow; they accumulate.  A pre-existing user-installed
 * discovery callback is preserved and chained.
 *
 * @param wf  Workflow handle (opaque).
 * @param sk  Skill handle.
 * @return 0 on success, -1 on NULL args.
 */
int csilk_ai_skill_register(csilk_wf_t* wf, csilk_ai_skill_t* sk);

/**
 * @brief Programmatically invoke a registered skill (equivalent to the model
 *        calling the `use_skill` trigger tool).
 *
 * Resolves the skill named by @p args_json (`{"name":"...","resource":"..."}`)
 * on @p wf and returns its loaded L2 instructions — or the L3 resource when
 * @p resource is present — as a heap-allocated JSON string.
 *
 * @param wf        Workflow the skill is registered on.
 * @param args_json Tool arguments JSON (may be `{}` for L2).
 * @return Heap-allocated JSON result (caller frees), or an error envelope.
 */
char* csilk_ai_skill_run_use_skill(csilk_wf_t* wf, const char* args_json);

/* --- Remote execution (cloud cluster, csilk_mq) --- */

/**
 * @brief Register a skill worker handler on an MQ instance for remote L3
 *        invocation.
 *
 * Subscribes to `csilk.skill.invoke` on @p mq (once per MQ), dispatches
 * matching requests by skill name to `csilk_skill_exec_local()` (or the skill's
 * L3 loader if no exec config is set), and publishes the JSON envelope back onto
 * `csilk.skill.result` tagged with the request's `idem` key.  Idempotent
 * duplicate invocations reuse the cached result envelope without re-executing.
 *
 * @param sk  Skill handle whose name and local exec config / loader serve remote calls.
 * @param mq  Message queue handle (must not be NULL).
 * @return 0 on success, -1 on NULL args / capacity exceeded.
 */
int csilk_ai_skill_worker_register(csilk_ai_skill_t* sk, csilk_mq_t* mq);

/**
 * @brief Unregister a skill from remote worker dispatch (called automatically
 *        by csilk_ai_skill_free()).
 */
void csilk_ai_skill_worker_unregister(csilk_ai_skill_t* sk);

/**
 * @brief Offload a skill's L3 resource execution to the cluster and wait for
 *        the result via csilk_mq.
 *
 * Publishes on `csilk.skill.invoke` and awaits `csilk.skill.result` with an
 * idempotent key and a bounded timeout.  When no remote worker responds
 * within @p timeout_ms (or none is registered), returns -1 with a timeout
 * error envelope in @p out_result.  Not usable from an I/O worker thread that
 * must stay lock-free; intended for the ReAct orchestrator or background tasks.
 *
 * @param sk            Skill handle (must be CSILK_SKILL_REMOTE).
 * @param mq            Message queue handle (cluster bus).
 * @param resource_path L3 resource to execute (e.g. "scripts/run.sh").
 * @param args          JSON / CLI arguments forwarded to the worker.
 * @param timeout_ms    Bounded wait (<= 0 uses a framework default).
 * @param[out] out_result [out] Heap-allocated result (caller frees), or an
 *                error envelope on timeout / failure.
 * @return 0 on success, -1 on failure / timeout.
 */
int csilk_ai_skill_invoke_remote(csilk_ai_skill_t* sk,
                                 csilk_mq_t*       mq,
                                 const char*       resource_path,
                                 const char*       args,
                                 int               timeout_ms,
                                 char**            out_result);

/**
 * @brief Configuration for local L3 script execution.
 *
 * A skill's L3 script is executed in a *bounded* local process: `fork` +
 * `execvp`, working directory confined to @p root (path-traversal guard),
 * a non-inheriting minimal environment, stdout capture capped at @p
 * max_output_bytes, and a bounded wall-clock wait (@p timeout_ms) that
 * SIGKILLs the child on expiry — the same "remote must failover, never
 * hang indefinitely" principle that governs csilk's HTTP clients.
 *
 * The framework does *not* build a process sandbox (seccomp/container);
 * isolation that matters for untrusted scripts is the deployment layer's
 * job.  What this config guarantees is: relative-path containment, bounded
 * resources, and no env leakage.
 */
typedef struct {
    char* root;             /**< Working-directory root; @p path must stay inside it. */
    int   timeout_ms;       /**< Bounded wait for the child (<= 0 → default). */
    int   max_output_bytes; /**< stdout capture cap (0 → framework default). */
    char* env;              /**< Optional space-separated KEY=VALUE env for the child. */
} csilk_skill_exec_cfg_t;

/**
 * @brief Attach a local-execution configuration to a skill.
 * @param sk   Skill handle.
 * @param cfg  Non-NULL config; fields are *copied* (caller keeps ownership).
 * @return 0 on success, -1 on NULL args / OOM.
 */
int csilk_ai_skill_set_exec_cfg(csilk_ai_skill_t* sk, const csilk_skill_exec_cfg_t* cfg);

/**
 * @brief Execute a skill's L3 resource as a local process.
 *
 * Runs @p path (resolved against the skill's exec-cfg root) with @p args
 * (space-separated argv, or NULL) under the exec config's bounds.  The
 * result is a JSON envelope:
 * ```json
 * {"skill":"..","path":"..","exit_code":0,"stdout":"...","truncated":false}
 * ```
 * On timeout the child is SIGKILLed and `exit_code` is -9.  On a path that
 * escapes the root, -1 is returned and @p out_result gets an error envelope.
 *
 * @param sk           Skill handle (must have an exec config set).
 * @param path         Resource path, relative to the exec root (e.g. "scripts/run.sh").
 * @param args         Space-separated argument string (NULL for no args).
 * @param timeout_ms   Bounded wait; <= 0 uses the exec config's default.
 * @param[out] out_result Heap-allocated JSON result (caller frees); set to NULL on failure.
 * @return 0 on success, -1 on failure.
 */
int csilk_skill_exec_local(
    csilk_ai_skill_t* sk, const char* path, const char* args, int timeout_ms, char** out_result);

#ifdef __cplusplus
}
#endif
