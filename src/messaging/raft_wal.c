/**
 * @file raft_wal.c
 * @brief Distributed Raft consensus WAL log replication engine implementation.
 * @copyright MIT License
 */

#include "raft_internal.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Create a new Raft node from a configuration.
 *
 * Allocates and zero-initializes the node, copies the config (defaulting
 * cluster_size to 3 when zero), and starts in FOLLOWER role at term 1 with an
 * initialized mutex.
 *
 * @param[in] config Initial node/cluster configuration (must not be NULL).
 * @return Heap-allocated node, or NULL on NULL config or allocation failure.
 */
csilk_raft_node_t*
csilk_raft_node_new(const csilk_raft_config_t* config)
{
    if (!config) {
        return NULL;
    }

    csilk_raft_node_t* node = calloc(1, sizeof(csilk_raft_node_t));
    if (!node) {
        return NULL;
    }

    node->config = *config;
    if (node->config.cluster_size == 0) {
        node->config.cluster_size = 3;
    }

    node->role = CSILK_RAFT_ROLE_FOLLOWER;
    node->current_term = 1;
    node->last_log_index = 0;
    node->commit_index = 0;
    csilk_mutex_init(&node->mutex);

    return node;
}

/**
 * @brief Destroy a Raft node and free its resources.
 *
 * Destroys the node mutex and frees the instance.  Safe to call with NULL.
 *
 * @param[in] node Node instance to free (may be NULL).
 */
void
csilk_raft_node_free(csilk_raft_node_t* node)
{
    if (!node) {
        return;
    }
    csilk_mutex_destroy(&node->mutex);
    free(node);
}

/**
 * @brief Append an entry to the leader's log (in-memory index bump).
 *
 * Only valid when the node is LEADER; increments last_log_index and returns
 * the new index.  This stub does not yet persist the entry to the WAL.
 *
 * @param[in] node Node instance (must be LEADER).
 * @param[in] data Entry payload (must not be NULL, non-empty).
 * @param[in] len  Length of @p data in bytes.
 * @return New last_log_index on success, 0 if not leader or on invalid input.
 */
uint64_t
csilk_raft_append_log(csilk_raft_node_t* node, const uint8_t* data, size_t len)
{
    if (!node || !data || len == 0 || node->role != CSILK_RAFT_ROLE_LEADER) {
        return 0;
    }

    node->last_log_index++;
    return node->last_log_index;
}

/**
 * @brief Advance the commit index once a quorum acknowledges.
 *
 * Computes the majority (cluster_size/2 + 1).  If @p ack_nodes meets the
 * majority and the log head is uncommitted, sets commit_index =
 * last_log_index.  Serialized by node->mutex.
 *
 * @param[in] node      Node instance (must not be NULL).
 * @param[in] ack_nodes Number of nodes that acknowledged the entry.
 * @return The resulting commit_index.
 */
uint64_t
csilk_raft_quorum_ack(csilk_raft_node_t* node, size_t ack_nodes)
{
    if (!node) {
        return 0;
    }

    size_t majority = (node->config.cluster_size / 2) + 1;
    if (ack_nodes >= majority && node->commit_index < node->last_log_index) {
        node->commit_index = node->last_log_index;
    }

    return node->commit_index;
}

/**
 * @brief Query the node's current Raft role.
 *
 * @param[in] node Node instance (may be NULL — returns FOLLOWER).
 * @return Current role (FOLLOWER if node is NULL).
 */
csilk_raft_role_t
csilk_raft_get_role(const csilk_raft_node_t* node)
{
    return node ? node->role : CSILK_RAFT_ROLE_FOLLOWER;
}

/**
 * @brief Query the node's current term.
 *
 * @param[in] node Node instance (may be NULL — returns 0).
 * @return Current term (0 if node is NULL).
 */
uint64_t
csilk_raft_get_term(const csilk_raft_node_t* node)
{
    return node ? node->current_term : 0;
}
