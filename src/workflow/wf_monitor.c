/**
 * @file wf_monitor.c
 * @brief Workflow monitoring: WebSocket-based real-time event broadcasting.
 */

#include "workflow_internal.h"

/* --- Monitoring --- */

/**
 * @brief Registers a WebSocket context client to receive real-time workflow monitor events.
 *
 * Registered monitor clients are sent structured JSON messages for event occurrences
 * (e.g. node execution starts/finishes, workflow paused/ended).
 *
 * @param wf The workflow instance.
 * @param c  Pointer to the WebSocket client context.
 */
void
csilk_wf_register_monitor(csilk_wf_t* wf, csilk_ctx_t* c)
{
    if (!wf || !c) {
        return;
    }
    csilk_mutex_lock(&wf->monitor_mutex);
    if (wf->monitor_count >= wf->monitor_capacity) {
        size_t        new_cap = wf->monitor_capacity == 0 ? 4 : wf->monitor_capacity * 2;
        csilk_ctx_t** new_monitors = realloc(wf->monitors, sizeof(csilk_ctx_t*) * new_cap);
        if (new_monitors) {
            wf->monitors = new_monitors;
            wf->monitor_capacity = new_cap;
        }
    }
    if (wf->monitor_count < wf->monitor_capacity) {
        wf->monitors[wf->monitor_count++] = c;
    }
    csilk_mutex_unlock(&wf->monitor_mutex);
}

/** @brief Internal: broadcast a JSON event to all registered WebSocket monitors.
 *
 * Constructs a JSON payload with event type, optional node_id, and optional
 * data payload string. The message is sent to each monitor via
 * csilk_ws_send(). Dead or closed connections are silently skipped.
 *
 * @param wf      Workflow instance.
 * @param event   Event name (e.g., "node_start", "workflow_end").
 * @param node_id Node identifier (may be nullptr for workflow-level events).
 * @param payload Additional event data string (may be nullptr). */
static void
broadcast_monitor_event(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload)
{
    if (wf->monitor_count == 0) {
        return;
    }
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "event", event);
    if (node_id) {
        cJSON_AddStringToObject(msg, "node_id", node_id);
    }
    if (payload) {
        cJSON_AddStringToObject(msg, "data", payload);
    }
    char* json = cJSON_PrintUnformatted(msg);
    csilk_mutex_lock(&wf->monitor_mutex);
    for (size_t i = 0; i < wf->monitor_count; i++) {
        if (csilk_is_sse(wf->monitors[i])) {
            csilk_sse_send(wf->monitors[i], event, json);
        } else {
            csilk_ws_send(wf->monitors[i], (uint8_t*)json, strlen(json), 0x1);
        }
    }
    csilk_mutex_unlock(&wf->monitor_mutex);
    free(json);
    cJSON_Delete(msg);
}

CSILK_INTERNAL void
_wf_broadcast(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload)
{
    broadcast_monitor_event(wf, event, node_id, payload);
}
