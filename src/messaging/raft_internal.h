/**
 * @file raft_internal.h
 * @brief Internal structures, frames, and definitions for Raft consensus engine.
 */

#ifndef CSILK_RAFT_INTERNAL_H
#define CSILK_RAFT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/sync.h"
#include "csilk/messaging/raft.h"

#define CSILK_RAFT_MAGIC 0x52414654 /* "RAFT" Magic Header */

typedef enum {
    CSILK_RAFT_MSG_REQUEST_VOTE_REQ = 0x01,
    CSILK_RAFT_MSG_REQUEST_VOTE_RESP = 0x02,
    CSILK_RAFT_MSG_APPEND_ENTRIES_REQ = 0x03,
    CSILK_RAFT_MSG_APPEND_ENTRIES_RESP = 0x04,
    CSILK_RAFT_MSG_INSTALL_SNAPSHOT_REQ = 0x05,
    CSILK_RAFT_MSG_INSTALL_SNAPSHOT_RESP = 0x06
} csilk_raft_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;       /**< 0x52414654 */
    uint8_t  msg_type;    /**< csilk_raft_msg_type_t */
    uint32_t payload_len; /**< Payload length in bytes */
    uint64_t term;        /**< Current term */
} csilk_raft_hdr_t;

typedef struct {
    char     peer_id[64];
    char     peer_addr[128];
    uint64_t next_index;
    uint64_t match_index;
} csilk_raft_peer_t;

struct csilk_raft_node_s {
    csilk_mutex_t       mutex;
    csilk_raft_role_t   role;
    csilk_raft_config_t config;

    uint64_t current_term;
    char     voted_for[64];
    void*    wal;

    uint64_t commit_index;
    uint64_t last_log_index;
    uint64_t last_applied;

    csilk_raft_peer_t peers[16];
    size_t            peer_count;

    uint64_t election_timeout_ms;
    uint64_t last_heartbeat_time;
};

/** @brief Create a snapshot of the current committed state. */
int
csilk_raft_snapshot_create(csilk_raft_node_t* node, const uint8_t* state_data, size_t state_len);

/** @brief Restore node state from a snapshot. */
int csilk_raft_snapshot_restore(csilk_raft_node_t* node,
                                const uint8_t*     snapshot_data,
                                size_t             snapshot_len);

#endif /* CSILK_RAFT_INTERNAL_H */
