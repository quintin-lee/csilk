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
 * @brief Callback invoked in log order when a log entry becomes committed.
 */
typedef void (*csilk_raft_apply_cb_t)(
    uint64_t index, uint64_t term, const uint8_t* data, size_t len, void* user_data);

/**
 * @brief Callback invoked when a snapshot is installed or recovered on startup.
 */
typedef void (*csilk_raft_snapshot_apply_cb_t)(
    uint64_t last_index, uint64_t last_term, const uint8_t* data, size_t len, void* user_data);

/**
 * @brief Registers a peer node into the cluster topology.
 * @param node Local node instance.
 * @param peer_id Peer node ID string.
 * @param peer_addr Peer IP/hostname address string ("host:port").
 * @return 0 on success, negative value on failure.
 */
int csilk_raft_node_add_peer(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr);

/**
 * @brief Binds a UDP consensus RPC socket on @p bind_addr ("host:port", ":0" or "127.0.0.1:0").
 * @param node Local node instance.
 * @param bind_addr Bind address string.
 * @return 0 on success, -1 on error.
 */
int csilk_raft_node_listen(csilk_raft_node_t* node, const char* bind_addr);

/**
 * @brief Returns the bound UDP consensus port (useful when bound to port 0).
 * @param node Local node instance.
 * @return Port number in host byte order, or 0 if not listening.
 */
uint16_t csilk_raft_node_get_port(const csilk_raft_node_t* node);

/**
 * @brief Starts Raft consensus event loop and heartbeat timers.
 * @param node Node instance.
 * @return 0 on success, negative value on failure.
 */
int csilk_raft_node_start(csilk_raft_node_t* node);

/**
 * @brief Stops the background Raft consensus transport thread (if running).
 * @param node Node instance.
 * @return 0 on success, -1 if @p node is NULL.
 */
int csilk_raft_node_stop(csilk_raft_node_t* node);

/**
 * @brief Advances Raft election, heartbeat, and WAL interval-sync timers manually.
 * @param node   Node instance.
 * @param now_ms Current monotonic timestamp in milliseconds (0 = query OS monotonic clock).
 * @return 0 on success, -1 on invalid args.
 */
int csilk_raft_node_tick(csilk_raft_node_t* node, uint64_t now_ms);

/**
 * @brief Appends a log entry as Leader and waits for majority commit acknowledgment.
 * @param node       Leader Raft node.
 * @param data       Payload bytes.
 * @param len        Payload length.
 * @param timeout_ms Maximum wait time in milliseconds.
 * @param out_index  Optional committed log index output.
 * @return 0 on quorum commit, -1 on failure/not leader/timeout.
 */
int csilk_raft_replicate_wait(csilk_raft_node_t* node,
                              const uint8_t*     data,
                              size_t             len,
                              uint32_t           timeout_ms,
                              uint64_t*          out_index);

/**
 * @brief Registers state machine apply callbacks and optional user context.
 * @param node        Raft node instance.
 * @param on_entry    Callback for committed log entries (may be NULL).
 * @param on_snapshot Callback for installed/recovered snapshots (may be NULL).
 * @param user_data   Opaque context passed to callbacks.
 * @return 0 on success, -1 on NULL node.
 */
int csilk_raft_set_apply_cb(csilk_raft_node_t*             node,
                            csilk_raft_apply_cb_t          on_entry,
                            csilk_raft_snapshot_apply_cb_t on_snapshot,
                            void*                          user_data);

/**
 * @brief Replays recovered snapshot and committed log entries through the apply callbacks.
 * @param node Raft node instance.
 * @return 0 on success, -1 on NULL node.
 */
int csilk_raft_replay_committed(csilk_raft_node_t* node);

/**
 * @brief Creates an atomic snapshot up to commit_index and compacts prior WAL entries.
 * @param node       Raft node instance.
 * @param state_data Serialized state machine snapshot bytes.
 * @param state_len  Length of @p state_data in bytes.
 * @return 0 on success, -1 on error.
 */
int
csilk_raft_snapshot_create(csilk_raft_node_t* node, const uint8_t* state_data, size_t state_len);

/**
 * @brief Restores snapshot state onto the Raft node.
 * @param node         Raft node instance.
 * @param snapshot_data Snapshot bytes.
 * @param snapshot_len  Length of @p snapshot_data in bytes.
 * @return 0 on success, -1 on error.
 */
int csilk_raft_snapshot_restore(csilk_raft_node_t* node,
                                const uint8_t*     snapshot_data,
                                size_t             snapshot_len);

/**
 * @brief Loads the latest snapshot metadata and bytes from the node.
 * @param[in]  node           Raft node instance.
 * @param[out] out_last_index Snapshot last included index.
 * @param[out] out_last_term  Snapshot last included term.
 * @param[out] out_data       Heap-allocated snapshot bytes (caller frees, may be NULL if none).
 * @param[out] out_len        Snapshot byte length.
 * @return 0 if a snapshot exists, -1 if none or invalid args.
 */
int csilk_raft_snapshot_load(const csilk_raft_node_t* node,
                             uint64_t*                out_last_index,
                             uint64_t*                out_last_term,
                             uint8_t**                out_data,
                             size_t*                  out_len);

/**
 * @brief Binds Workflow Manager to Raft node for cluster state replication.
 * @param mgr Workflow manager instance.
 * @param raft_node Raft node instance.
 * @return 0 on success, negative value on failure.
 */
int csilk_wf_cluster_bind_raft(csilk_wf_manager_t* mgr, csilk_raft_node_t* raft_node);

/**
 * @brief Replicates a cluster workflow command into the Leader Raft WAL.
 */
int csilk_wf_cluster_replicate_cmd(csilk_raft_node_t* node, uint8_t cmd_type, const char* payload);

/**
 * @brief Replicates a cluster workflow command and waits for quorum commit.
 */
int csilk_wf_cluster_replicate_cmd_wait(csilk_raft_node_t* node,
                                        uint8_t            cmd_type,
                                        const char*        payload,
                                        uint32_t           timeout_ms);

/**
 * @brief Returns total number of Raft-committed commands applied to @p mgr.
 */
size_t csilk_wf_cluster_applied_count(const csilk_wf_manager_t* mgr);

/**
 * @brief Copies the most recent Raft-committed command applied to @p mgr.
 */
int csilk_wf_cluster_get_last_cmd(const csilk_wf_manager_t* mgr,
                                  uint8_t*                  out_cmd_type,
                                  char*                     out_payload,
                                  size_t                    out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_RAFT_H */
