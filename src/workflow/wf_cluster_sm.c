/**
 * @file wf_cluster_sm.c
 * @brief Cluster state-machine integration binding workflows to Raft for
 *        distributed consensus, command replication, and snapshot replay.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/core/sync.h"
#include "csilk/messaging/raft.h"
#include "workflow_internal.h"

#define WF_CLUSTER_MAX_BINDINGS 32
#define WF_CLUSTER_PAYLOAD_CAP 1024

typedef struct {
    csilk_wf_manager_t* mgr;
    csilk_raft_node_t*  node;
    size_t              applied_count;
    uint8_t             has_last_cmd;
    uint8_t             last_cmd_type;
    char                last_payload[WF_CLUSTER_PAYLOAD_CAP];
    uint64_t            last_applied_index;
} wf_cluster_binding_t;

static wf_cluster_binding_t s_bindings[WF_CLUSTER_MAX_BINDINGS];
static size_t               s_binding_count = 0;
static csilk_mutex_t        s_binding_mutex;
static csilk_once_t         s_binding_once = CSILK_ONCE_INIT;

static void
wf_cluster_init_mutex(void)
{
    csilk_mutex_init(&s_binding_mutex);
}

static void
wf_sm_try_apply_dsl(csilk_wf_manager_t* mgr, const char* payload)
{
    if (!mgr || !payload || payload[0] != '{') {
        return;
    }
    csilk_wf_t* wf = csilk_wf_from_json_ext(payload, NULL, 0);
    if (!wf) {
        return;
    }
    const char* name = (wf->name && wf->name[0]) ? wf->name : "default";
    if (csilk_wf_manager_get(mgr, name) != NULL) {
        if (csilk_wf_manager_reload(mgr, name, wf) != 0) {
            csilk_wf_free(wf);
        }
    } else {
        if (csilk_wf_manager_register(mgr, name, wf) != 0) {
            csilk_wf_free(wf);
        }
    }
}

static void
wf_sm_on_entry(uint64_t index, uint64_t term, const uint8_t* data, size_t len, void* user_data)
{
    (void)term;
    wf_cluster_binding_t* slot = (wf_cluster_binding_t*)user_data;
    if (!slot || !data || len == 0) {
        return;
    }

    csilk_wf_manager_t* mgr = NULL;
    char                payload_copy[WF_CLUSTER_PAYLOAD_CAP];
    payload_copy[0] = '\0';

    csilk_mutex_lock(&s_binding_mutex);
    if (slot->mgr != NULL && index > slot->last_applied_index) {
        slot->last_applied_index = index;
        slot->applied_count++;
        slot->has_last_cmd = 1;
        slot->last_cmd_type = data[0];

        size_t plen = len - 1U;
        if (plen >= sizeof(slot->last_payload)) {
            plen = sizeof(slot->last_payload) - 1U;
        }
        if (plen > 0) {
            memcpy(slot->last_payload, data + 1U, plen);
        }
        slot->last_payload[plen] = '\0';
        mgr = slot->mgr;
        memcpy(payload_copy, slot->last_payload, plen + 1U);
    }
    csilk_mutex_unlock(&s_binding_mutex);

    if (mgr && payload_copy[0] == '{') {
        wf_sm_try_apply_dsl(mgr, payload_copy);
    }
}

static void
wf_sm_on_snapshot(
    uint64_t last_index, uint64_t last_term, const uint8_t* data, size_t len, void* user_data)
{
    (void)last_term;
    wf_cluster_binding_t* slot = (wf_cluster_binding_t*)user_data;
    if (!slot || !data || len == 0) {
        return;
    }

    csilk_mutex_lock(&s_binding_mutex);
    if (slot->mgr != NULL && last_index >= slot->last_applied_index) {
        slot->last_applied_index = last_index;
        slot->has_last_cmd = 1;
        if (len >= 2U) {
            slot->last_cmd_type = data[0];
            size_t plen = len - 1U;
            if (plen >= sizeof(slot->last_payload)) {
                plen = sizeof(slot->last_payload) - 1U;
            }
            memcpy(slot->last_payload, data + 1U, plen);
            slot->last_payload[plen] = '\0';
        } else {
            slot->last_cmd_type = data[0];
            slot->last_payload[0] = '\0';
        }
        if (slot->applied_count == 0) {
            slot->applied_count = 1;
        }
    }
    csilk_mutex_unlock(&s_binding_mutex);
}

void
_wf_cluster_unbind_mgr(csilk_wf_manager_t* mgr)
{
    if (!mgr) {
        return;
    }

    csilk_once(&s_binding_once, wf_cluster_init_mutex);
    csilk_mutex_lock(&s_binding_mutex);
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].mgr == mgr) {
            memset(&s_bindings[i], 0, sizeof(s_bindings[i]));
        }
    }
    csilk_mutex_unlock(&s_binding_mutex);
}

int
csilk_wf_cluster_bind_raft(csilk_wf_manager_t* mgr, csilk_raft_node_t* raft_node)
{
    if (!mgr || !raft_node) {
        return -1;
    }

    csilk_once(&s_binding_once, wf_cluster_init_mutex);
    csilk_mutex_lock(&s_binding_mutex);

    wf_cluster_binding_t* slot = NULL;
    wf_cluster_binding_t* free_slot = NULL;
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].mgr == mgr) {
            slot = &s_bindings[i];
            break;
        }
        if (s_bindings[i].mgr == NULL && !free_slot) {
            free_slot = &s_bindings[i];
        }
    }

    if (!slot) {
        if (free_slot) {
            slot = free_slot;
        } else {
            if (s_binding_count >= WF_CLUSTER_MAX_BINDINGS) {
                csilk_mutex_unlock(&s_binding_mutex);
                return -1;
            }
            slot = &s_bindings[s_binding_count++];
        }
        memset(slot, 0, sizeof(*slot));
        slot->mgr = mgr;
    } else if (slot->node != raft_node) {
        slot->applied_count = 0;
        slot->has_last_cmd = 0;
        slot->last_cmd_type = 0;
        slot->last_payload[0] = '\0';
        slot->last_applied_index = 0;
    }
    slot->node = raft_node;
    csilk_mutex_unlock(&s_binding_mutex);

    if (csilk_raft_set_apply_cb(raft_node, wf_sm_on_entry, wf_sm_on_snapshot, slot) != 0) {
        return -1;
    }
    if (csilk_raft_replay_committed(raft_node) != 0) {
        return -1;
    }
    return 0;
}

static uint8_t*
wf_encode_cmd_frame(uint8_t cmd_type, const char* payload, size_t* out_len)
{
    size_t   plen = strlen(payload);
    size_t   flen = plen + 1U;
    uint8_t* frame = malloc(flen);
    if (!frame) {
        return NULL;
    }
    frame[0] = cmd_type;
    if (plen > 0) {
        memcpy(frame + 1U, payload, plen);
    }
    *out_len = flen;
    return frame;
}

int
csilk_wf_cluster_replicate_cmd(csilk_raft_node_t* node, uint8_t cmd_type, const char* payload)
{
    if (!node || !payload) {
        return -1;
    }

    size_t   flen = 0;
    uint8_t* frame = wf_encode_cmd_frame(cmd_type, payload, &flen);
    if (!frame) {
        return -1;
    }

    uint64_t idx = csilk_raft_append_log(node, frame, flen);
    free(frame);

    return idx > 0 ? 0 : -1;
}

int
csilk_wf_cluster_replicate_cmd_wait(csilk_raft_node_t* node,
                                    uint8_t            cmd_type,
                                    const char*        payload,
                                    uint32_t           timeout_ms)
{
    if (!node || !payload) {
        return -1;
    }

    size_t   flen = 0;
    uint8_t* frame = wf_encode_cmd_frame(cmd_type, payload, &flen);
    if (!frame) {
        return -1;
    }

    int rc = csilk_raft_replicate_wait(node, frame, flen, timeout_ms, NULL);
    free(frame);
    return rc;
}

size_t
csilk_wf_cluster_applied_count(const csilk_wf_manager_t* mgr)
{
    if (!mgr) {
        return 0;
    }
    csilk_once(&s_binding_once, wf_cluster_init_mutex);
    csilk_mutex_lock(&s_binding_mutex);
    size_t count = 0;
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].mgr == mgr) {
            count = s_bindings[i].applied_count;
            break;
        }
    }
    csilk_mutex_unlock(&s_binding_mutex);
    return count;
}

int
csilk_wf_cluster_get_last_cmd(const csilk_wf_manager_t* mgr,
                              uint8_t*                  out_cmd_type,
                              char*                     out_payload,
                              size_t                    out_cap)
{
    if (!mgr) {
        return -1;
    }
    csilk_once(&s_binding_once, wf_cluster_init_mutex);
    csilk_mutex_lock(&s_binding_mutex);
    int rc = -1;
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].mgr == mgr && s_bindings[i].has_last_cmd) {
            if (out_cmd_type) {
                *out_cmd_type = s_bindings[i].last_cmd_type;
            }
            if (out_payload && out_cap > 0) {
                snprintf(out_payload, out_cap, "%s", s_bindings[i].last_payload);
            }
            rc = 0;
            break;
        }
    }
    csilk_mutex_unlock(&s_binding_mutex);
    return rc;
}
