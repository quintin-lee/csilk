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

/**
 * @brief Unregisters a WebSocket context client from receiving workflow monitor events.
 *
 * @param wf The workflow instance.
 * @param c  Pointer to the WebSocket client context to unregister.
 */
void
csilk_wf_unregister_monitor(csilk_wf_t* wf, csilk_ctx_t* c)
{
    if (!wf || !c) {
        return;
    }
    csilk_mutex_lock(&wf->monitor_mutex);
    for (size_t i = 0; i < wf->monitor_count; i++) {
        if (wf->monitors[i] == c) {
            for (size_t j = i; j + 1 < wf->monitor_count; j++) {
                wf->monitors[j] = wf->monitors[j + 1];
            }
            wf->monitor_count--;
            break;
        }
    }
    csilk_mutex_unlock(&wf->monitor_mutex);
}

/** @brief Internal: broadcast a JSON event to all registered WebSocket monitors.
 *
 * Constructs a JSON payload with event type, optional node_id, and optional
 * data payload string. The message is sent to each monitor via
 * csilk_ws_send(). Dead or closed connections are silently skipped and pruned.
 *
 * @param wf      Workflow instance.
 * @param event   Event name (e.g., "node_start", "workflow_end").
 * @param node_id Node identifier (may be NULL for workflow-level events).
 * @param payload Additional event data string (may be NULL). */
static void
broadcast_monitor_event(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload)
{
    if (wf->monitor_count == 0) {
        return;
    }
    csilk_json_t* msg = csilk_json_object();
    if (!msg || csilk_json_add_string(msg, "event", event) != 0 ||
        (node_id && csilk_json_add_string(msg, "node_id", node_id) != 0) ||
        (payload && csilk_json_add_string(msg, "data", payload) != 0)) {
        csilk_json_free(msg);
        return;
    }
    char* json = csilk_json_serialize(msg, NULL);
    if (!json) {
        csilk_json_free(msg);
        return;
    }
    csilk_mutex_lock(&wf->monitor_mutex);
    for (size_t i = 0; i < wf->monitor_count;) {
        csilk_ctx_t* mc = wf->monitors[i];
        if (csilk_ctx_is_closed(mc)) {
            for (size_t j = i; j + 1 < wf->monitor_count; j++) {
                wf->monitors[j] = wf->monitors[j + 1];
            }
            wf->monitor_count--;
            continue;
        }
        if (csilk_is_sse(mc)) {
            int sse_result = csilk_sse_send(mc, event, json);
            if (sse_result < 0) {
                for (size_t j = i; j + 1 < wf->monitor_count; j++) {
                    wf->monitors[j] = wf->monitors[j + 1];
                }
                wf->monitor_count--;
                continue;
            }
        } else {
            csilk_ws_send(mc, (uint8_t*)json, strlen(json), 0x1);
        }
        i++;
    }
    csilk_mutex_unlock(&wf->monitor_mutex);
    free(json);
    csilk_json_free(msg);
}

/**
 * @brief Broadcasts an event to all registered workflow monitors.
 *
 * Wrapper that forwards the event to broadcast_monitor_event(), emitting a
 * structured JSON message (via WebSocket or SSE) to every monitor client.
 *
 * @param wf      The workflow instance.
 * @param event   Event name (e.g. "node_start", "workflow_end").
 * @param node_id Originating node id (may be NULL for workflow-level events).
 * @param payload Optional event data string (may be NULL).
 */
CSILK_INTERNAL void
_wf_broadcast(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload)
{
    broadcast_monitor_event(wf, event, node_id, payload);
}
