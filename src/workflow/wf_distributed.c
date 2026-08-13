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

/* --- Registry for Distributed Workflows --- */
static csilk_wf_t* g_distributed_wfs[32];
static size_t      g_distributed_wf_count = 0;

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
        const char* node_id = j_node_id ? csilk_json_string_value(j_node_id) : NULL;
        const char* output_str = csilk_json_string_value(j_output);

        for (size_t i = 0; i < g_distributed_wf_count; i++) {
            csilk_wf_t* wf = g_distributed_wfs[i];

            // 1. Check for Active Context (Hot Resume)
            csilk_wf_ctx_t* active = _wf_find_active_ctx(wf, exec_id);
            if (active) {
                CSILK_LOG_I("Workflow found active execution %s, resuming hot", exec_id);
                csilk_wf_node_t* n = node_id ? csilk_wf_get_node(wf, node_id) : NULL;
                // If node_id wasn't provided, we might have to search the context for a
                // paused node

                if (n) {
                    csilk_mutex_lock(&active->mutex);
                    active->node_approved[n->index] = 1;
                    active->nodes_active--; // Decrement the count we added during offload
                    csilk_mutex_unlock(&active->mutex);

                    csilk_data_t* out_data = csilk_wf_data_new(
                        active, "application/json", csilk_wf_strdup(active, output_str));
                    _wf_execute_node(active, n, out_data);
                    break;
                }
            }

            // 2. Fallback to Cold Resume (WAL)
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, exec_id);
            if (access(path, F_OK) == 0) {
                CSILK_LOG_I("Workflow found WAL for %s, signaling continue (cold)", exec_id);
                csilk_data_t out_data = {"application/json", (void*)output_str, NULL, NULL};
                csilk_wf_signal_continue(wf, exec_id, &out_data, NULL);
                break;
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
    if (g_distributed_wf_count < 32) {
        g_distributed_wfs[g_distributed_wf_count++] = wf;
    }
    csilk_mq_subscribe(mq, "csilk.wf.results", on_remote_result);
    CSILK_LOG_I("Workflow: enabled distributed execution on message queue topic "
                "'csilk.wf.results' for workflow '%s'",
                wf->name);
}
