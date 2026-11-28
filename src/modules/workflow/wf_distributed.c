/**
 * @file wf_distributed.c
 * @brief Distributed workflow execution, node coordination, and remote step handling.
 */

#include "workflow_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"

#include <unistd.h>

/* Forward declaration -- defined in wf_graph.c */
csilk_wf_node_t* csilk_wf_get_node(csilk_wf_t* wf, const char* id);

/* --- Shared Workflow Pointer Registry & Distributed Registry --- */
static wf_ptr_registry_t g_distributed_reg = {0};
static csilk_mutex_t     g_wf_reg_mutex;
static csilk_once_t      g_wf_reg_once = CSILK_ONCE_INIT;

static void
wf_reg_sync_init(void)
{
    csilk_mutex_init(&g_wf_reg_mutex);
}

CSILK_INTERNAL void
_wf_ptr_registry_add(wf_ptr_registry_t* reg, void* item)
{
    if (!reg || !item) {
        return;
    }
    csilk_once(&g_wf_reg_once, wf_reg_sync_init);
    csilk_mutex_lock(&g_wf_reg_mutex);
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->items[i] == item) {
            csilk_mutex_unlock(&g_wf_reg_mutex);
            return;
        }
    }
    if (reg->count < CSILK_WF_PTR_REG_CAP) {
        reg->items[reg->count++] = item;
    }
    csilk_mutex_unlock(&g_wf_reg_mutex);
}

CSILK_INTERNAL void
_wf_ptr_registry_remove(wf_ptr_registry_t* reg, const void* item)
{
    if (!reg || !item) {
        return;
    }
    csilk_once(&g_wf_reg_once, wf_reg_sync_init);
    csilk_mutex_lock(&g_wf_reg_mutex);
    for (size_t i = 0; i < reg->count;) {
        if (reg->items[i] == item) {
            for (size_t j = i; j + 1 < reg->count; j++) {
                reg->items[j] = reg->items[j + 1];
            }
            reg->count--;
        } else {
            i++;
        }
    }
    csilk_mutex_unlock(&g_wf_reg_mutex);
}

CSILK_INTERNAL size_t
_wf_ptr_registry_snapshot(wf_ptr_registry_t* reg,
                          void**             out_items,
                          size_t             cap,
                          void (*on_take)(void* item))
{
    if (!reg || !out_items || cap == 0) {
        return 0;
    }
    csilk_once(&g_wf_reg_once, wf_reg_sync_init);
    csilk_mutex_lock(&g_wf_reg_mutex);
    size_t n = 0;
    for (size_t i = 0; i < reg->count && n < cap; i++) {
        out_items[n] = reg->items[i];
        if (on_take && out_items[n]) {
            on_take(out_items[n]);
        }
        n++;
    }
    csilk_mutex_unlock(&g_wf_reg_mutex);
    return n;
}

static void
wf_pending_ref(void* item)
{
    csilk_wf_t* wf = (csilk_wf_t*)item;
    atomic_fetch_add(&wf->pending_completions, 1);
}

CSILK_INTERNAL int
_wf_is_safe_exec_id(const char* exec_id)
{
    if (!exec_id || exec_id[0] == '\0') {
        return 0;
    }
    size_t len = strlen(exec_id);
    if (len >= 128) {
        return 0;
    }
    if (strstr(exec_id, "..") != NULL) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = exec_id[i];
        if (c == '/' || c == '\\' || (unsigned char)c < 0x20 || (unsigned char)c == 0x7f) {
            return 0;
        }
    }
    return 1;
}

CSILK_INTERNAL void
_wf_distributed_unregister(csilk_wf_t* wf)
{
    _wf_ptr_registry_remove(&g_distributed_reg, wf);
}

static void
on_remote_result(csilk_mq_ctx_t* m_ctx)
{
    size_t      len;
    const char* payload = csilk_mq_get_payload(m_ctx, &len);
    if (!payload) {
        csilk_mq_next(m_ctx);
        return;
    }
    CSILK_LOG_I("Workflow received remote result: %s", payload);

    csilk_json_t* root = csilk_json_parse(payload);
    if (!root) {
        csilk_mq_next(m_ctx);
        return;
    }

    csilk_json_t* j_exec_id = csilk_json_get(root, "exec_id");
    csilk_json_t* j_node_id = csilk_json_get(root, "node_id");
    csilk_json_t* j_output = csilk_json_get(root, "output");

    if (j_exec_id && csilk_json_is_string(j_exec_id) && j_output &&
        csilk_json_is_string(j_output)) {
        const char* exec_id = csilk_json_string_value(j_exec_id);
        const char* node_id = (j_node_id && csilk_json_is_string(j_node_id))
                                  ? csilk_json_string_value(j_node_id)
                                  : NULL;
        const char* output_str = csilk_json_string_value(j_output);

        if (_wf_is_safe_exec_id(exec_id)) {
            void*  snap_raw[CSILK_WF_PTR_REG_CAP];
            size_t snap_count = _wf_ptr_registry_snapshot(
                &g_distributed_reg, snap_raw, CSILK_WF_PTR_REG_CAP, wf_pending_ref);
            csilk_wf_t** snap = (csilk_wf_t**)snap_raw;

            for (size_t i = 0; i < snap_count; i++) {
                csilk_wf_t* wf = snap[i];

                // 1. Check for Active Context (Hot Resume)
                csilk_wf_ctx_t* active = _wf_find_active_ctx(wf, exec_id);
                if (active) {
                    CSILK_LOG_I("Workflow found active execution %s, resuming hot", exec_id);
                    csilk_wf_node_t* n = node_id ? csilk_wf_get_node(wf, node_id) : NULL;

                    if (n) {
                        csilk_mutex_lock(&active->mutex);
                        active->node_approved[n->index] = 1;
                        atomic_fetch_sub(&active->nodes_active, 1);
                        csilk_mutex_unlock(&active->mutex);

                        char* copied = output_str ? csilk_wf_strdup(active, output_str) : NULL;
                        csilk_data_t* out_data =
                            csilk_wf_data_new(active, "application/json", copied);
                        _wf_execute_node(active, n, out_data);
                        break;
                    }
                }

                // 2. Fallback to Cold Resume (WAL)
                if (wf->wal_dir) {
                    char path[512];
                    int  plen = snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, exec_id);
                    if (plen > 0 && (size_t)plen < sizeof(path) && access(path, F_OK) == 0) {
                        CSILK_LOG_I("Workflow found WAL for %s, signaling continue (cold)",
                                    exec_id);
                        csilk_data_t out_data = {"application/json", (void*)output_str, NULL, NULL};
                        csilk_wf_signal_continue(wf, exec_id, &out_data, NULL);
                        break;
                    }
                }
            }

            for (size_t i = 0; i < snap_count; i++) {
                atomic_fetch_sub(&snap[i]->pending_completions, 1);
            }
        }
    }

    csilk_json_free(root);
    csilk_mq_next(m_ctx);
}

/**
 * @brief Enables distributed workflow support by binding a message queue connection.
 *
 * Subscribes to the "csilk.wf.results" topic to dynamically receive and resume offloaded tasks.
 *
 * @param wf The workflow instance.
 * @param mq The message queue connection handle.
 */
void
csilk_wf_enable_distributed(csilk_wf_t* wf, csilk_mq_t* mq)
{
    if (!wf || !mq || !wf->wal_dir) {
        return;
    }
    wf->mq = mq;
    wf->dist_unregister_fn = _wf_distributed_unregister;
    _wf_ptr_registry_add(&g_distributed_reg, wf);
    csilk_mq_subscribe(mq, "csilk.wf.results", on_remote_result);
    CSILK_LOG_I("Workflow: enabled distributed execution on message queue topic "
                "'csilk.wf.results' for workflow '%s'",
                wf->name);
}
