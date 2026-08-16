/**
 * @file wf_wal.c
 * @brief WAL event logging for workflow execution.
 */

#include <string.h>
#include <stdlib.h>

#include "workflow_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"

/** @brief Internal: persist a workflow event to the Write-Ahead Log.
 *
 * Packs node_id, data type, and data value into a flat payload buffer
 * and delegates to _wf_wal_append(). The payload format is:
 *   [node_id\0][data_type\0][data_value\0]
 * Fields are NUL-terminated strings for simple parsing during recovery.
 *
 * @param ctx     Workflow execution context (must have wal_path set).
 * @param type    Event type (WF_EV_START, WF_EV_NODE_START, etc.).
 * @param node_id Originating node ID, or NULL for workflow-level events.
 * @param data    Associated data (may be NULL for simple events).
 * @note This is a no-op if ctx has no WAL path configured. */
void
_wf_wal_log_event(csilk_wf_ctx_t*       ctx,
                  csilk_wf_event_type_t type,
                  const char*           node_id,
                  csilk_data_t*         data)
{
    if (!ctx->wal_path) {
        return;
    }

    /* Ensure we always have 3 null-terminated strings for consistency,
     even if some fields are NULL. This prevents buffer overflows during
     recovery parsing. */
    const char* nid = node_id ? node_id : "";
    const char* d_type = (data && data->type) ? data->type : "";
    const char* d_val = (data && data->value) ? (char*)data->value : "";

    size_t node_id_len = strlen(nid) + 1;
    size_t type_len = strlen(d_type) + 1;
    size_t val_len = strlen(d_val) + 1;

    size_t total_len = node_id_len + type_len + val_len;
    char*  payload = malloc(total_len);
    if (!payload) {
        return;
    }

    char* p = payload;
    memcpy(p, nid, node_id_len);
    p += node_id_len;
    memcpy(p, d_type, type_len);
    p += type_len;
    memcpy(p, d_val, val_len);

    _wf_wal_append(ctx->wal_path, type, payload, total_len);
    free(payload);
}
