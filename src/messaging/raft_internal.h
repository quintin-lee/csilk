/**
 * @file raft_internal.h
 * @brief Internal structures, frames, and definitions for Raft consensus engine.
 */

#ifndef CSILK_RAFT_INTERNAL_H
#define CSILK_RAFT_INTERNAL_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#include "csilk/core/sync.h"
#include "csilk/messaging/raft.h"

#define CSILK_RAFT_MAGIC 0x52414654      /* "RAFT" RPC Magic Header */
#define CSILK_RAFT_META_MAGIC 0x524D4554 /* "RMET" Metadata Magic */
#define CSILK_RAFT_LOG_MAGIC 0x524C4F47  /* "RLOG" Log Record Magic */
#define CSILK_RAFT_SNAP_MAGIC 0x52534E50 /* "RSNP" Snapshot Magic */
#define CSILK_RAFT_FORMAT_VERSION 1U
#define CSILK_RAFT_MAX_PEERS 16
#define CSILK_RAFT_MAX_FRAME_DATA (64U * 1024U * 1024U)
#define CSILK_RAFT_SNAPSHOT_CHUNK_SIZE 8192U
#define CSILK_RAFT_RPC_MAX_PACKET 65507U

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

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t current_term;
    uint32_t has_voted_for;
    char     voted_for[64];
    uint32_t crc32;
} csilk_raft_meta_disk_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint64_t index;
    uint64_t term;
    uint32_t data_len;
    uint32_t hdr_crc32;
} csilk_raft_wal_rec_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t last_included_index;
    uint64_t last_included_term;
    uint64_t state_len;
    uint32_t hdr_crc32;
} csilk_raft_snap_disk_hdr_t;

typedef struct {
    uint64_t index;
    uint64_t term;
    uint8_t* data;
    size_t   len;
    uint64_t wal_offset;
} csilk_raft_log_entry_t;

typedef struct {
    char               peer_id[64];
    char               peer_addr[128];
    uint64_t           next_index;
    uint64_t           match_index;
    uint8_t            vote_granted;
    uint64_t           snapshot_offset;
    struct sockaddr_in addr_in;
    int                addr_resolved;
} csilk_raft_peer_t;

typedef struct {
    uint64_t term;
    uint32_t candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
    char     candidate_str[64];
} csilk_raft_rv_req_t;

typedef struct {
    uint64_t term;
    uint32_t voter_id;
    uint8_t  vote_granted;
} csilk_raft_rv_resp_t;

typedef struct {
    uint64_t term;
    uint32_t leader_id;
    uint64_t prev_log_index;
    uint64_t prev_log_term;
    uint64_t leader_commit;
    uint32_t entry_count;
} csilk_raft_ae_req_t;

typedef struct {
    uint64_t term;
    uint32_t follower_id;
    uint8_t  success;
    uint64_t match_index;
    uint64_t conflict_index;
    uint64_t conflict_term;
} csilk_raft_ae_resp_t;

typedef struct {
    uint64_t term;
    uint32_t leader_id;
    uint64_t last_included_index;
    uint64_t last_included_term;
    uint64_t total_len;
    uint64_t offset;
    uint32_t chunk_len;
    uint8_t  done;
} csilk_raft_is_req_t;

typedef struct {
    uint64_t term;
    uint32_t follower_id;
    uint64_t next_offset;
    uint8_t  success;
} csilk_raft_is_resp_t;

struct csilk_raft_node_s {
    csilk_mutex_t       mutex;
    csilk_cond_t        commit_cond;
    csilk_raft_role_t   role;
    csilk_raft_config_t config;

    uint64_t current_term;
    char     voted_for[64];
    void*    wal;

    /* Persistence paths and WAL file state */
    char*    wal_dir;
    char*    meta_path;
    char*    wal_path;
    char*    snap_path;
    int      wal_fd;
    uint64_t wal_file_size;
    size_t   pending_sync_entries;
    uint64_t last_sync_ms;
    uint64_t sync_count;

    /* In-memory log suffix (indices > log_base_index) */
    csilk_raft_log_entry_t* entries;
    size_t                  entry_count;
    size_t                  entry_capacity;
    uint64_t                log_base_index;
    uint64_t                log_base_term;

    /* Snapshot state and chunked InstallSnapshot reassembly buffer */
    uint8_t* snapshot_data;
    size_t   snapshot_len;
    uint64_t snapshot_last_index;
    uint64_t snapshot_last_term;

    uint8_t* snap_rx_buf;
    size_t   snap_rx_total;
    size_t   snap_rx_received;
    uint64_t snap_rx_index;
    uint64_t snap_rx_term;

    uint64_t commit_index;
    uint64_t last_log_index;
    uint64_t last_applied;

    csilk_raft_peer_t peers[CSILK_RAFT_MAX_PEERS];
    size_t            peer_count;

    uint64_t election_timeout_ms;
    uint64_t last_heartbeat_time;
    uint64_t last_broadcast_time;

    /* Transport loop state */
    int            listen_fd;
    uint16_t       listen_port;
    char           listen_addr[128];
    csilk_thread_t transport_thread;
    int            transport_running;
    int            transport_stop;
    int            started;
    uint32_t       rng_state;

    /* State machine apply callbacks */
    csilk_raft_apply_cb_t          apply_cb;
    csilk_raft_snapshot_apply_cb_t snapshot_apply_cb;
    void*                          apply_user_data;
};

/* --- Shared WAL durability helpers (wal_common.c) --- */
uint32_t _csilk_wal_crc32(uint32_t crc, const void* data, size_t len);
int      _csilk_wal_write_all(int fd, const void* buf, size_t len);
int      _csilk_wal_read_all(int fd, void* buf, size_t len);
int      _csilk_wal_fsync_fd(int fd);
int      _csilk_wal_fsync_dir(const char* file_path);
int      _csilk_wal_mkdir_p(const char* dir);
int      _csilk_wal_write_atomic(const char* path, const void* data, size_t len);
uint64_t _csilk_raft_now_ms(void);

/* --- Raft WAL internal operations (raft_wal.c) --- */
int  _raft_wal_open_and_recover(csilk_raft_node_t* node);
void _raft_wal_close(csilk_raft_node_t* node);
int  _raft_wal_save_meta_locked(csilk_raft_node_t* node);
int  _raft_wal_append_locked(
     csilk_raft_node_t* node, uint64_t term, const uint8_t* data, size_t len, uint64_t* out_index);
int      _raft_wal_truncate_from_locked(csilk_raft_node_t* node, uint64_t from_index);
int      _raft_wal_compact_prefix_locked(csilk_raft_node_t* node,
                                         uint64_t           last_included_index,
                                         uint64_t           last_included_term);
int      _raft_wal_flush_locked(csilk_raft_node_t* node);
uint64_t _raft_log_term_at_locked(const csilk_raft_node_t* node, uint64_t index);
const csilk_raft_log_entry_t* _raft_log_get_locked(const csilk_raft_node_t* node, uint64_t index);

/* --- Raft Snapshot internal operations (raft_snapshot.c) --- */
int _raft_snapshot_load_disk_locked(csilk_raft_node_t* node);
int _raft_snapshot_install_locked(csilk_raft_node_t* node,
                                  uint64_t           last_included_index,
                                  uint64_t           last_included_term,
                                  const uint8_t*     data,
                                  size_t             len);

/* --- Raft RPC framing (raft_rpc.c) --- */
int csilk_raft_rpc_encode_hdr(csilk_raft_msg_type_t msg_type,
                              uint64_t              term,
                              uint32_t              payload_len,
                              uint8_t*              out_buf,
                              size_t                out_cap);
int csilk_raft_rpc_decode_hdr(const uint8_t* buf, size_t len, csilk_raft_hdr_t* out_hdr);

int _raft_rpc_encode_rv_req(const csilk_raft_rv_req_t* req, uint8_t* out_buf, size_t out_cap);
int _raft_rpc_decode_rv_req(const uint8_t* buf, size_t len, csilk_raft_rv_req_t* out_req);
int _raft_rpc_encode_rv_resp(const csilk_raft_rv_resp_t* resp, uint8_t* out_buf, size_t out_cap);
int _raft_rpc_decode_rv_resp(const uint8_t* buf, size_t len, csilk_raft_rv_resp_t* out_resp);

int _raft_rpc_encode_ae_req(const csilk_raft_ae_req_t*    req,
                            const csilk_raft_log_entry_t* entries,
                            size_t                        entry_count,
                            uint8_t*                      out_buf,
                            size_t                        out_cap);
int _raft_rpc_decode_ae_req(const uint8_t*           buf,
                            size_t                   len,
                            csilk_raft_ae_req_t*     out_req,
                            csilk_raft_log_entry_t** out_entries,
                            size_t*                  out_count);
int _raft_rpc_encode_ae_resp(const csilk_raft_ae_resp_t* resp, uint8_t* out_buf, size_t out_cap);
int _raft_rpc_decode_ae_resp(const uint8_t* buf, size_t len, csilk_raft_ae_resp_t* out_resp);

int _raft_rpc_encode_is_req(const csilk_raft_is_req_t* req,
                            const uint8_t*             chunk,
                            uint8_t*                   out_buf,
                            size_t                     out_cap);
int _raft_rpc_decode_is_req(const uint8_t*       buf,
                            size_t               len,
                            csilk_raft_is_req_t* out_req,
                            const uint8_t**      out_chunk);
int _raft_rpc_encode_is_resp(const csilk_raft_is_resp_t* resp, uint8_t* out_buf, size_t out_cap);
int _raft_rpc_decode_is_resp(const uint8_t* buf, size_t len, csilk_raft_is_resp_t* out_resp);

/* --- Raft Consensus step/dispatch (raft_consensus.c) --- */
void _raft_apply_committed_locked(csilk_raft_node_t* node);
int  _raft_handle_rpc_packet(csilk_raft_node_t*        node,
                             const uint8_t*            pkt,
                             size_t                    pkt_len,
                             const struct sockaddr_in* src_addr,
                             uint8_t*                  out_reply,
                             size_t                    out_cap,
                             size_t*                   out_reply_len);
void _raft_broadcast_append_entries_locked(csilk_raft_node_t* node);

#endif /* CSILK_RAFT_INTERNAL_H */
