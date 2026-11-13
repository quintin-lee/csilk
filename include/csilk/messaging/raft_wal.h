#pragma once
/**
 * @file raft_wal.h
 * @brief Distributed Raft consensus WAL log replication engine for csilk.
 *
 * @version 0.5.2
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
 * @brief WAL fsync durability policy for Raft persistence.
 */
typedef enum {
    CSILK_RAFT_SYNC_ALWAYS = 0,     /**< fsync/fdatasync on every log append (default). */
    CSILK_RAFT_SYNC_BATCH = 1,      /**< fsync when batch threshold is reached or on flush. */
    CSILK_RAFT_SYNC_INTERVAL_MS = 2 /**< fsync when sync_interval_ms elapses or on flush. */
} csilk_raft_sync_policy_t;

/**
 * @brief Configuration options for Raft consensus node.
 */
typedef struct {
    uint32_t    node_id;      /**< Unique ID of this node. */
    size_t      cluster_size; /**< Total number of nodes in cluster. */
    const char* wal_dir;      /**< Directory for WAL, meta & snapshots (NULL = in-memory). */
    csilk_raft_sync_policy_t sync_policy; /**< WAL fsync durability policy (0 = ALWAYS). */
    uint32_t sync_interval_ms;            /**< Sync interval for INTERVAL_MS policy (0 = 100ms). */
    size_t   sync_batch_size;             /**< Batch size for BATCH policy (0 = 16). */
    uint64_t heartbeat_interval_ms;       /**< Leader heartbeat interval (0 = 50ms). */
    uint64_t election_timeout_min_ms;     /**< Min election timeout (0 = 150ms). */
    uint64_t election_timeout_max_ms;     /**< Max election timeout (0 = 300ms). */
} csilk_raft_config_t;

/**
 * @brief Create a new Raft consensus WAL node.
 * @param config Node configuration options.
 * @return New Raft node instance, or NULL on failure.
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
 * @brief Flush and fsync any buffered Raft WAL records to durable storage.
 * @param node Raft node instance.
 * @return 0 on success, -1 on error.
 */
int csilk_raft_wal_flush(csilk_raft_node_t* node);

/**
 * @brief Get total number of WAL fsync/fdatasync calls performed by this node.
 * @param node Raft node instance.
 * @return Sync counter (0 if in-memory or NULL).
 */
uint64_t csilk_raft_wal_sync_count(const csilk_raft_node_t* node);

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

/**
 * @brief Get highest log index known to be committed on this node.
 * @param node Raft node instance.
 * @return Commit index (0 if none or NULL).
 */
uint64_t csilk_raft_get_commit_index(const csilk_raft_node_t* node);

/**
 * @brief Get highest log index stored on this node (including compacted prefix).
 * @param node Raft node instance.
 * @return Last log index (0 if empty or NULL).
 */
uint64_t csilk_raft_get_last_log_index(const csilk_raft_node_t* node);

/**
 * @brief Get term of the last log entry (or snapshot base term if compacted).
 * @param node Raft node instance.
 * @return Last log term (0 if empty or NULL).
 */
uint64_t csilk_raft_get_last_log_term(const csilk_raft_node_t* node);

/**
 * @brief Get highest log index applied to the local state machine.
 * @param node Raft node instance.
 * @return Last applied index (0 if none or NULL).
 */
uint64_t csilk_raft_get_last_applied(const csilk_raft_node_t* node);

/**
 * @brief Read a retained (uncompacted) log entry at @p index.
 *
 * Allocates a copy of the payload in @p out_data (caller must free()).
 *
 * @param[in]  node     Raft node instance.
 * @param[in]  index    1-based log index (> snapshot base index).
 * @param[out] out_term Optional term output.
 * @param[out] out_data Optional heap-allocated payload copy (caller frees).
 * @param[out] out_len  Optional payload length output.
 * @return 0 on success, -1 if not found / compacted / invalid args.
 */
int csilk_raft_get_log_entry(const csilk_raft_node_t* node,
                             uint64_t                 index,
                             uint64_t*                out_term,
                             uint8_t**                out_data,
                             size_t*                  out_len);

#ifdef __cplusplus
}
#endif
