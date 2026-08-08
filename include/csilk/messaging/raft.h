/**
 * @file raft.h
 * @brief Public API header for Raft consensus engine and cluster HA.
 */

#ifndef CSILK_RAFT_H
#define CSILK_RAFT_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/messaging/raft_wal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers a peer node into the cluster topology.
 * @param node Local node instance.
 * @param peer_id Peer node ID string.
 * @param peer_addr Peer IP/hostname address string.
 * @return 0 on success, negative value on failure.
 */
int csilk_raft_node_add_peer(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr);

/**
 * @brief Starts Raft consensus event loop and heartbeat timers.
 * @param node Node instance.
 * @return 0 on success, negative value on failure.
 */
int csilk_raft_node_start(csilk_raft_node_t* node);

/**
 * @brief Queries current role of the local node (Follower, Candidate, or Leader).
 * @param node Node instance.
 * @return Current role enum.
 */
csilk_raft_role_t csilk_raft_get_role(const csilk_raft_node_t* node);

/**
 * @brief Binds Workflow Manager to Raft node for cluster state replication.
 * @param mgr Workflow manager instance.
 * @param raft_node Raft node instance.
 * @return 0 on success, negative value on failure.
 */
int csilk_wf_cluster_bind_raft(csilk_wf_manager_t* mgr, csilk_raft_node_t* raft_node);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_RAFT_H */
