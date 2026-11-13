/**
 * @file wf_cluster_sm.c
 * @brief Cluster state-machine integration binding workflows to Raft for
 *        distributed consensus and command replication.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/core/sync.h"
#include "csilk/messaging/raft.h"

#define WF_CLUSTER_MAX_BINDINGS 32

typedef struct {
    csilk_wf_manager_t* mgr;
    csilk_raft_node_t*  node;
} wf_cluster_binding_t;

static wf_cluster_binding_t s_bindings[WF_CLUSTER_MAX_BINDINGS];
static size_t               s_binding_count = 0;
static csilk_mutex_t        s_binding_mutex;
static int                  s_binding_mutex_ready = 0;

static void
ensure_binding_mutex(void)
{
    if (!s_binding_mutex_ready) {
        csilk_mutex_init(&s_binding_mutex);
        s_binding_mutex_ready = 1;
    }
}

/**
 * @brief Binds a workflow manager to a Raft node for cluster coordination.
 *
 * Validates the arguments and records the association in the cluster registry.
 *
 * @param mgr       The workflow manager to bind (must not be NULL).
 * @param raft_node The Raft node to associate with the manager (must not be NULL).
 * @return 0 on success, or -1 if either argument is NULL or table full.
 */
int
csilk_wf_cluster_bind_raft(csilk_wf_manager_t* mgr, csilk_raft_node_t* raft_node)
{
    if (!mgr || !raft_node) {
        return -1;
    }

    ensure_binding_mutex();
    csilk_mutex_lock(&s_binding_mutex);

    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].mgr == mgr) {
            s_bindings[i].node = raft_node;
            csilk_mutex_unlock(&s_binding_mutex);
            return 0;
        }
    }

    if (s_binding_count >= WF_CLUSTER_MAX_BINDINGS) {
        csilk_mutex_unlock(&s_binding_mutex);
        return -1;
    }

    s_bindings[s_binding_count].mgr = mgr;
    s_bindings[s_binding_count].node = raft_node;
    s_binding_count++;

    csilk_mutex_unlock(&s_binding_mutex);
    return 0;
}

/**
 * @brief Replicates a cluster command through Raft for distributed workflows.
 *
 * Encodes the command type and payload into a binary frame and appends it
 * into the Raft node's write-ahead log.
 *
 * @param node      The Raft node that should replicate the command (must not be NULL).
 * @param cmd_type  Opaque command type identifier.
 * @param payload   Command payload bytes (must not be NULL).
 * @return 0 on success, or -1 on NULL args / replication failure.
 */
int
csilk_wf_cluster_replicate_cmd(csilk_raft_node_t* node, uint8_t cmd_type, const char* payload)
{
    if (!node || !payload) {
        return -1;
    }

    size_t   plen = strlen(payload);
    size_t   flen = plen + 1;
    uint8_t* frame = malloc(flen);
    if (!frame) {
        return -1;
    }
    frame[0] = cmd_type;
    memcpy(frame + 1, payload, plen);

    uint64_t idx = csilk_raft_append_log(node, frame, flen);
    free(frame);

    return idx > 0 ? 0 : -1;
}
