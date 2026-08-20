/**
 * @file wf_resume.c
 * @brief Workflow resumption: replaying the Write-Ahead Log to restore
 *        execution state and re-trigger unfinished or paused nodes.
 *
 * @copyright MIT License
 */

#include "workflow_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"

/**
 * @brief Resumes a paused or interrupted workflow execution from its WAL.
 *
 * Flushes the WAL, drops any stale active context for exec_id, and replays the
 * workflow's .wal file to reconstruct node outputs, input counts, pause and
 * end state. Finished nodes are skipped; unfinished (started-but-not-finished
 * or not-yet-started) nodes are re-executed with their reconstructed inputs.
 * If the WAL records an end, the callback is invoked with NULL and the context
 * is freed; if paused, the context is discarded.
 *
 * @param wf        The workflow definition (must have wal_dir configured).
 * @param exec_id   The unique execution ID whose WAL should be replayed.
 * @param callback  Completion callback receiving the final result (NULL for
 *                  paused/ended executions).
 * @note No-op if wf, exec_id, or wf->wal_dir is NULL. Not thread-safe with
 *       respect to concurrent execution of the same exec_id.
 */
void
csilk_wf_resume(csilk_wf_t* wf, const char* exec_id, void (*callback)(csilk_data_t* result))
{
    if (!wf || !exec_id || !wf->wal_dir) {
        return;
    }

    _wf_wal_flush();

    _wf_cleanup_stale_ctx(wf, exec_id);

    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
    FILE* f = fopen(wal_path, "rb");
    if (!f) {
        return;
    }
    csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
    atomic_init(&ctx->nodes_active, 0);
    ctx->wf = wf;
    ctx->callback = callback;
    ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
    ctx->node_approved = calloc(wf->node_count, sizeof(int));
    ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
    ctx->arena = csilk_arena_new(0);
    csilk_mutex_init(&ctx->mutex);
    csilk_mutex_init(&ctx->arena_mutex);
    csilk_mutex_init(&ctx->trace_mutex);
    snprintf(ctx->exec_id, sizeof(ctx->exec_id), "%s", exec_id);
    ctx->wal_path = strdup(wal_path);
    int *node_started = calloc(wf->node_count, sizeof(int)),
        *node_finished = calloc(wf->node_count, sizeof(int));
    int                   wf_ended = 0;
    csilk_wf_wal_header_t header;
    while (fread(&header, sizeof(header), 1, f) == 1) {
        if (header.magic != CSILK_WF_MAGIC) {
            break;
        }
        char* payload = header.payload_len > 0 ? malloc(header.payload_len) : NULL;
        if (payload && fread(payload, header.payload_len, 1, f) != 1) {
            free(payload);
            break;
        }
        switch (header.type) {
        case WF_EV_NODE_START:
            if (!payload) {
                break;
            }
            for (size_t i = 0; i < wf->node_count; i++) {
                if (strcmp(wf->nodes[i]->id, payload) == 0) {
                    node_started[i] = 1;
                    break;
                }
            }
            break;
        case WF_EV_NODE_FINISH: {
            if (!payload) {
                break;
            }
            char*  node_id = payload;
            size_t nid_len = strlen(node_id);
            if (nid_len + 1 >= header.payload_len) {
                break;
            }
            char*  data_type = node_id + nid_len + 1;
            size_t dtype_len = strlen(data_type);
            if (nid_len + 1 + dtype_len + 1 >= header.payload_len) {
                break;
            }
            char* data_val = data_type + dtype_len + 1;

            for (size_t i = 0; i < wf->node_count; i++) {
                if (strcmp(wf->nodes[i]->id, node_id) == 0) {
                    node_finished[i] = 1;
                    ctx->node_outputs[i] =
                        csilk_wf_data_new(ctx, data_type, csilk_wf_strdup(ctx, data_val));
                    csilk_wf_node_t* n = wf->nodes[i];
                    for (size_t j = 0; j < n->edge_count; j++) {
                        ctx->node_input_counts[n->edges[j].target->index]++;
                    }
                    break;
                }
            }
            break;
        }
        case WF_EV_PAUSE: {
            if (!payload) {
                break;
            }
            for (size_t i = 0; i < wf->node_count; i++) {
                if (strcmp(wf->nodes[i]->id, payload) == 0) {
                    ctx->is_paused = 1;
                    break;
                }
            }
            break;
        }
        case WF_EV_END:
            wf_ended = 1;
            break;
        }
        free(payload);
    }
    fclose(f);
    if (wf_ended) {
        if (callback) {
            callback(NULL);
        }
        _wf_cleanup_ctx(ctx);
    } else if (ctx->is_paused) {
        _wf_cleanup_ctx(ctx);
    } else {
        for (size_t i = 0; i < wf->node_count; i++) {
            csilk_wf_node_t* n = wf->nodes[i];
            int              trigger = 0;
            csilk_data_t*    trigger_input = ctx->initial_input;
            if (node_started[i] && !node_finished[i]) {
                trigger = 1;
            } else if (!node_started[i]) {
                int threshold = n->incoming_count == 0 ? 1 : n->incoming_count;
                if (ctx->node_input_counts[n->index] >= threshold) {
                    trigger = 1;
                }
            }
            if (trigger) {
                for (size_t j = 0; j < wf->node_count; j++) {
                    csilk_wf_node_t* p = wf->nodes[j];
                    for (size_t k = 0; k < p->edge_count; k++) {
                        if (p->edges[k].target == n && node_finished[j]) {
                            trigger_input = ctx->node_outputs[j];
                            break;
                        }
                    }
                }
                _wf_execute_node(ctx, n, trigger_input);
            }
        }
    }
    free(node_started);
    free(node_finished);
}

/**
 * @brief Continues a paused workflow execution after a human/remote result.
 *
 * Flushes the WAL and replays it to find the node at which execution paused
 * (WF_EV_PAUSE). Marks that node as approved and re-executes it with the
 * supplied input (the human/remote-provided result). If no paused node is
 * found the reconstructed context is freed without executing further.
 *
 * @param wf        The workflow definition (must have wal_dir configured).
 * @param exec_id   The unique execution ID to continue.
 * @param input     The result data delivered to the paused node.
 * @param callback  Completion callback receiving the final result.
 * @note No-op if wf, exec_id, or wf->wal_dir is NULL.
 */
void
csilk_wf_signal_continue(csilk_wf_t*   wf,
                         const char*   exec_id,
                         csilk_data_t* input,
                         void (*callback)(csilk_data_t* result))
{
    if (!wf || !exec_id || !wf->wal_dir) {
        return;
    }

    _wf_wal_flush();

    _wf_cleanup_stale_ctx(wf, exec_id);

    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
    FILE* f = fopen(wal_path, "rb");
    if (!f) {
        return;
    }

    csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
    atomic_init(&ctx->nodes_active, 0);
    ctx->wf = wf;
    ctx->callback = callback;
    ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
    ctx->node_approved = calloc(wf->node_count, sizeof(int));
    ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
    ctx->arena = csilk_arena_new(0);
    csilk_mutex_init(&ctx->mutex);
    csilk_mutex_init(&ctx->arena_mutex);
    csilk_mutex_init(&ctx->trace_mutex);
    snprintf(ctx->exec_id, sizeof(ctx->exec_id), "%s", exec_id);
    ctx->wal_path = strdup(wal_path);

    char* paused_node_id = NULL;

    csilk_wf_wal_header_t header;
    while (fread(&header, sizeof(header), 1, f) == 1) {
        if (header.magic != CSILK_WF_MAGIC) {
            break;
        }
        char* payload = header.payload_len > 0 ? malloc(header.payload_len) : NULL;
        if (payload && fread(payload, header.payload_len, 1, f) != 1) {
            free(payload);
            break;
        }
        switch (header.type) {
        case WF_EV_NODE_FINISH: {
            if (!payload) {
                break;
            }
            char*  node_id = payload;
            size_t nid_len = strlen(node_id);
            if (nid_len + 1 >= header.payload_len) {
                break;
            }
            char*  data_type = node_id + nid_len + 1;
            size_t dtype_len = strlen(data_type);
            if (nid_len + 1 + dtype_len + 1 >= header.payload_len) {
                break;
            }
            char* data_val = data_type + dtype_len + 1;

            for (size_t i = 0; i < wf->node_count; i++) {
                if (strcmp(wf->nodes[i]->id, node_id) == 0) {
                    ctx->node_outputs[i] =
                        csilk_wf_data_new(ctx, data_type, csilk_wf_strdup(ctx, data_val));
                    csilk_wf_node_t* n = wf->nodes[i];
                    for (size_t j = 0; j < n->edge_count; j++) {
                        ctx->node_input_counts[n->edges[j].target->index]++;
                    }
                    break;
                }
            }
            break;
        }
        case WF_EV_PAUSE:
            if (!payload) {
                break;
            }
            free(paused_node_id);
            paused_node_id = strdup(payload);
            break;
        case WF_EV_END:
            break;
        }
        free(payload);
    }
    fclose(f);

    if (paused_node_id) {
        csilk_wf_node_t* n = csilk_wf_get_node(wf, paused_node_id);
        if (n) {
            ctx->node_approved[n->index] = 1;
            _wf_execute_node(ctx, n, input);
        }
        free(paused_node_id);
    } else {
        _wf_cleanup_ctx(ctx);
    }
}
