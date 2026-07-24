#pragma once
/**
 * @file raft_wal.h
 * @brief Distributed Raft consensus WAL log replication engine for csilk.
 *
 * @version 0.5.0
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_raft_node_s csilk_raft_node_t;

/**
 * @brief Raft node role state.
 */
typedef enum {
    CSILK_RAFT_ROLE_FOLLOWER = 0,
    CSILK_RAFT_ROLE_CANDIDATE = 1,
    CSILK_RAFT_ROLE_LEADER = 2,
} csilk_raft_role_t;

/**
 * @brief Configuration options for Raft consensus node.
 */
typedef struct {
    uint32_t node_id;      /**< Unique ID of this node. */
    size_t   cluster_size; /**< Total number of nodes in cluster. */
} csilk_raft_config_t;

/**
 * @brief Create a new Raft consensus WAL node.
 * @param config Node configuration options.
 * @return New Raft node instance, or nullptr on failure.
 */
csilk_raft_node_t* csilk_raft_node_new(const csilk_raft_config_t* config);

/**
 * @brief Destroy a Raft consensus node instance.
 * @param node Raft node instance.
 */
void csilk_raft_node_free(csilk_raft_node_t* node);

/**
 * @brief Append a log entry to the Raft WAL engine (Leader only).
 * @param node Raft node instance.
 * @param data Log entry payload data.
 * @param len Payload length in bytes.
 * @return Log entry index on success, or 0 on failure/not leader.
 */
uint64_t csilk_raft_append_log(csilk_raft_node_t* node, const uint8_t* data, size_t len);

/**
 * @brief Simulate majority quorum acknowledgment and advance commit index.
 * @param node Raft node instance.
 * @param ack_nodes Number of nodes acknowledging log entry.
 * @return Current committed log index.
 */
uint64_t csilk_raft_quorum_ack(csilk_raft_node_t* node, size_t ack_nodes);

/**
 * @brief Get current role of Raft node.
 * @param node Raft node instance.
 * @return Node role enum.
 */
csilk_raft_role_t csilk_raft_get_role(const csilk_raft_node_t* node);

/**
 * @brief Get current term of Raft node.
 * @param node Raft node instance.
 * @return Current term counter.
 */
uint64_t csilk_raft_get_term(const csilk_raft_node_t* node);

#ifdef __cplusplus
}
#endif
