/**
 * @file raft_consensus.c
 * @brief Raft consensus node operations — peer membership and startup.
 *
 * Implements peer registration and node start.  Peers are stored in a fixed
 * 16-slot array guarded by node->mutex.  Starting a node with no peers elects
 * it leader immediately (single-node cluster) and bumps the current term.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

/**
 * @brief Register a peer node in the cluster topology.
 *
 * Appends the peer (id + address, copied via snprintf) to the node's peer
 * array, initializing its next_index to 1 and match_index to 0.  At most 16
 * peers are supported; the operation is serialized by node->mutex.
 *
 * @param[in] node      Local node instance (must not be NULL).
 * @param[in] peer_id   Peer node ID string (must not be NULL).
 * @param[in] peer_addr Peer address string (must not be NULL).
 * @return 0 on success, -1 on NULL arguments or when the peer table is full.
 */
int
csilk_raft_node_add_peer(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr)
{
    if (!node || !peer_id || !peer_addr) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    if (node->peer_count >= 16) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    csilk_raft_peer_t* peer = &node->peers[node->peer_count++];
    snprintf(peer->peer_id, sizeof(peer->peer_id), "%s", peer_id);
    snprintf(peer->peer_addr, sizeof(peer->peer_addr), "%s", peer_addr);
    peer->next_index = 1;
    peer->match_index = 0;

    csilk_mutex_unlock(&node->mutex);
    return 0;
}

/**
 * @brief Start the Raft node's consensus activity.
 *
 * If the node has no peers it becomes leader immediately (single-node quorum)
 * and increments current_term; otherwise it remains a follower awaiting
 * heartbeats/elections.  Serialized by node->mutex.
 *
 * @param[in] node The node instance (must not be NULL).
 * @return 0 on success, -1 if @p node is NULL.
 */
int
csilk_raft_node_start(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    if (node->peer_count == 0) {
        node->role = CSILK_RAFT_ROLE_LEADER;
        node->current_term++;
    }
    csilk_mutex_unlock(&node->mutex);
    return 0;
}
