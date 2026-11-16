/**
 * @file raft_consensus.c
 * @brief Raft consensus state machine, randomized elections, UDP RPC transport,
 *        log replication, and state-machine apply loop.
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "raft_internal.h"

#define RAFT_MAX_ENTRIES_PER_AE 32U
#define RAFT_AE_SAFE_MTU_BYTES 48000U

static uint32_t
raft_rng_next(uint32_t* state)
{
    uint32_t x = *state;
    if (x == 0) {
        x = 0x9E3779B9U;
    }
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

static uint64_t
raft_pick_timeout_locked(csilk_raft_node_t* node)
{
    uint64_t min_ms = node->config.election_timeout_min_ms;
    uint64_t max_ms = node->config.election_timeout_max_ms;
    if (max_ms <= min_ms) {
        return min_ms;
    }
    uint64_t span = max_ms - min_ms + 1ULL;
    return min_ms + (uint64_t)(raft_rng_next(&node->rng_state) % (uint32_t)span);
}

static size_t
raft_cluster_majority_locked(const csilk_raft_node_t* node)
{
    size_t total = node->config.cluster_size;
    if (total < node->peer_count + 1U) {
        total = node->peer_count + 1U;
    }
    if (total == 0) {
        total = 1;
    }
    return (total / 2U) + 1U;
}

static int
raft_parse_host_port(const char* addr_str, char* out_host, size_t host_cap, uint16_t* out_port)
{
    if (!addr_str || !out_host || host_cap == 0 || !out_port) {
        return -1;
    }

    const char* colon = strrchr(addr_str, ':');
    if (!colon || colon[1] == '\0') {
        return -1;
    }

    char*         endp = NULL;
    unsigned long p = strtoul(colon + 1, &endp, 10);
    if (!endp || *endp != '\0' || p > 65535UL) {
        return -1;
    }

    size_t hlen = (size_t)(colon - addr_str);
    if (hlen >= host_cap) {
        return -1;
    }
    if (hlen == 0) {
        out_host[0] = '\0';
    } else {
        memcpy(out_host, addr_str, hlen);
        out_host[hlen] = '\0';
    }
    *out_port = (uint16_t)p;
    return 0;
}

static int
raft_resolve_ipv4(const char* host, uint16_t port, int for_bind, struct sockaddr_in* out_addr)
{
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family = AF_INET;
    out_addr->sin_port = htons(port);

    if (!host || host[0] == '\0') {
        out_addr->sin_addr.s_addr = htonl(for_bind ? INADDR_ANY : INADDR_LOOPBACK);
        return 0;
    }

    if (strcmp(host, "localhost") == 0) {
        out_addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return 0;
    }

    if (inet_pton(AF_INET, host, &out_addr->sin_addr) == 1) {
        return 0;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return -1;
    }
    const struct sockaddr_in* sin = (const struct sockaddr_in*)res->ai_addr;
    out_addr->sin_addr = sin->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static int
raft_ensure_peer_resolved_locked(csilk_raft_peer_t* peer)
{
    if (peer->addr_resolved) {
        return 0;
    }
    char     host[128];
    uint16_t port = 0;
    if (raft_parse_host_port(peer->peer_addr, host, sizeof(host), &port) != 0 || port == 0) {
        return -1;
    }
    if (raft_resolve_ipv4(host, port, 0, &peer->addr_in) != 0) {
        return -1;
    }
    peer->addr_resolved = 1;
    return 0;
}

static csilk_raft_peer_t*
raft_find_peer_locked(csilk_raft_node_t*        node,
                      uint32_t                  peer_num_id,
                      const struct sockaddr_in* src_addr)
{
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%u", peer_num_id);

    for (size_t i = 0; i < node->peer_count; i++) {
        if (strcmp(node->peers[i].peer_id, id_buf) == 0) {
            return &node->peers[i];
        }
    }

    if (src_addr) {
        for (size_t i = 0; i < node->peer_count; i++) {
            if (raft_ensure_peer_resolved_locked(&node->peers[i]) == 0 &&
                node->peers[i].addr_in.sin_port == src_addr->sin_port &&
                node->peers[i].addr_in.sin_addr.s_addr == src_addr->sin_addr.s_addr) {
                return &node->peers[i];
            }
        }
    }
    return NULL;
}

static void
raft_send_packet_locked(csilk_raft_node_t* node,
                        csilk_raft_peer_t* peer,
                        const uint8_t*     pkt,
                        size_t             pkt_len)
{
    if (node->listen_fd < 0 || !pkt || pkt_len == 0) {
        return;
    }
    if (raft_ensure_peer_resolved_locked(peer) != 0) {
        return;
    }
    (void)sendto(node->listen_fd,
                 pkt,
                 pkt_len,
                 0,
                 (const struct sockaddr*)&peer->addr_in,
                 sizeof(peer->addr_in));
}

static void
raft_step_down_locked(csilk_raft_node_t* node, uint64_t new_term)
{
    node->current_term = new_term;
    node->role = CSILK_RAFT_ROLE_FOLLOWER;
    node->voted_for[0] = '\0';
    (void)_raft_wal_save_meta_locked(node);
}

void
_raft_apply_committed_locked(csilk_raft_node_t* node)
{
    if (!node) {
        return;
    }

    while (node->last_applied < node->commit_index) {
        uint64_t next_idx = node->last_applied + 1ULL;
        if (next_idx <= node->log_base_index) {
            node->last_applied = node->log_base_index;
            continue;
        }
        const csilk_raft_log_entry_t* ent = _raft_log_get_locked(node, next_idx);
        if (!ent) {
            break;
        }
        node->last_applied = next_idx;
        if (node->apply_cb) {
            node->apply_cb(ent->index, ent->term, ent->data, ent->len, node->apply_user_data);
        }
    }
}

static void
raft_advance_leader_commit_locked(csilk_raft_node_t* node)
{
    size_t majority = raft_cluster_majority_locked(node);
    for (uint64_t n = node->last_log_index; n > node->commit_index; n--) {
        if (_raft_log_term_at_locked(node, n) != node->current_term) {
            continue;
        }
        size_t ack = 1;
        for (size_t i = 0; i < node->peer_count; i++) {
            if (node->peers[i].match_index >= n) {
                ack++;
            }
        }
        if (ack >= majority) {
            node->commit_index = n;
            _raft_apply_committed_locked(node);
            csilk_cond_broadcast(&node->commit_cond);
            break;
        }
    }
}

static void
raft_send_install_snapshot_to_peer_locked(csilk_raft_node_t* node,
                                          csilk_raft_peer_t* peer,
                                          uint8_t*           tx_buf,
                                          size_t             tx_cap)
{
    if (!node->snapshot_data || node->snapshot_len == 0) {
        return;
    }

    uint64_t offset = peer->snapshot_offset;
    if (offset >= node->snapshot_len) {
        offset = 0;
        peer->snapshot_offset = 0;
    }

    size_t remain = node->snapshot_len - (size_t)offset;
    size_t chunk_len =
        remain > CSILK_RAFT_SNAPSHOT_CHUNK_SIZE ? CSILK_RAFT_SNAPSHOT_CHUNK_SIZE : remain;

    csilk_raft_is_req_t req;
    memset(&req, 0, sizeof(req));
    req.term = node->current_term;
    req.leader_id = node->config.node_id;
    req.last_included_index = node->snapshot_last_index;
    req.last_included_term = node->snapshot_last_term;
    req.total_len = (uint64_t)node->snapshot_len;
    req.offset = offset;
    req.chunk_len = (uint32_t)chunk_len;
    req.done = (offset + chunk_len >= node->snapshot_len) ? 1U : 0U;

    int plen = _raft_rpc_encode_is_req(&req,
                                       node->snapshot_data + offset,
                                       tx_buf + sizeof(csilk_raft_hdr_t),
                                       tx_cap - sizeof(csilk_raft_hdr_t));
    if (plen < 0) {
        return;
    }
    if (csilk_raft_rpc_encode_hdr(CSILK_RAFT_MSG_INSTALL_SNAPSHOT_REQ,
                                  node->current_term,
                                  (uint32_t)plen,
                                  tx_buf,
                                  tx_cap) < 0) {
        return;
    }
    raft_send_packet_locked(node, peer, tx_buf, sizeof(csilk_raft_hdr_t) + (size_t)plen);
}

void
_raft_broadcast_append_entries_locked(csilk_raft_node_t* node)
{
    if (!node || node->role != CSILK_RAFT_ROLE_LEADER) {
        return;
    }
    node->last_broadcast_time = _csilk_raft_now_ms();
    if (node->listen_fd < 0 || node->peer_count == 0) {
        return;
    }

    uint8_t* tx_buf = malloc(CSILK_RAFT_RPC_MAX_PACKET);
    if (!tx_buf) {
        return;
    }

    for (size_t i = 0; i < node->peer_count; i++) {
        csilk_raft_peer_t* peer = &node->peers[i];
        if (peer->next_index == 0) {
            peer->next_index = 1;
        }

        if (peer->next_index <= node->log_base_index && node->snapshot_data &&
            node->snapshot_len > 0) {
            raft_send_install_snapshot_to_peer_locked(
                node, peer, tx_buf, CSILK_RAFT_RPC_MAX_PACKET);
            continue;
        }

        uint64_t prev_idx = peer->next_index - 1ULL;
        if (prev_idx < node->log_base_index) {
            prev_idx = node->log_base_index;
        }
        uint64_t prev_term = _raft_log_term_at_locked(node, prev_idx);

        size_t start_pos = 0;
        if (prev_idx + 1ULL > node->log_base_index) {
            start_pos = (size_t)(prev_idx + 1ULL - node->log_base_index - 1ULL);
        }

        size_t count = 0;
        size_t est_bytes = sizeof(csilk_raft_hdr_t) + sizeof(csilk_raft_ae_req_t);
        while (start_pos + count < node->entry_count && count < RAFT_MAX_ENTRIES_PER_AE) {
            size_t ent_wire =
                sizeof(uint64_t) * 2U + sizeof(uint32_t) + node->entries[start_pos + count].len;
            if (est_bytes + ent_wire > RAFT_AE_SAFE_MTU_BYTES && count > 0) {
                break;
            }
            if (est_bytes + ent_wire > CSILK_RAFT_RPC_MAX_PACKET) {
                break;
            }
            est_bytes += ent_wire;
            count++;
        }

        csilk_raft_ae_req_t ae;
        memset(&ae, 0, sizeof(ae));
        ae.term = node->current_term;
        ae.leader_id = node->config.node_id;
        ae.prev_log_index = prev_idx;
        ae.prev_log_term = prev_term;
        ae.leader_commit = node->commit_index;
        ae.entry_count = (uint32_t)count;

        const csilk_raft_log_entry_t* slice = (count > 0) ? &node->entries[start_pos] : NULL;
        int                           plen = _raft_rpc_encode_ae_req(&ae,
                                           slice,
                                           count,
                                           tx_buf + sizeof(csilk_raft_hdr_t),
                                           CSILK_RAFT_RPC_MAX_PACKET - sizeof(csilk_raft_hdr_t));
        if (plen < 0) {
            continue;
        }
        if (csilk_raft_rpc_encode_hdr(CSILK_RAFT_MSG_APPEND_ENTRIES_REQ,
                                      node->current_term,
                                      (uint32_t)plen,
                                      tx_buf,
                                      CSILK_RAFT_RPC_MAX_PACKET) < 0) {
            continue;
        }
        raft_send_packet_locked(node, peer, tx_buf, sizeof(csilk_raft_hdr_t) + (size_t)plen);
    }

    free(tx_buf);
}

static void
raft_become_leader_locked(csilk_raft_node_t* node)
{
    node->role = CSILK_RAFT_ROLE_LEADER;
    for (size_t i = 0; i < node->peer_count; i++) {
        node->peers[i].next_index = node->last_log_index + 1ULL;
        node->peers[i].match_index = 0;
        node->peers[i].snapshot_offset = 0;
    }
    _raft_broadcast_append_entries_locked(node);
}

static void
raft_start_election_locked(csilk_raft_node_t* node, uint64_t now_ms)
{
    node->role = CSILK_RAFT_ROLE_CANDIDATE;
    node->current_term++;
    snprintf(node->voted_for, sizeof(node->voted_for), "%u", node->config.node_id);
    (void)_raft_wal_save_meta_locked(node);

    for (size_t i = 0; i < node->peer_count; i++) {
        node->peers[i].vote_granted = 0;
    }

    node->last_heartbeat_time = now_ms;
    node->election_timeout_ms = raft_pick_timeout_locked(node);

    size_t majority = raft_cluster_majority_locked(node);
    if (majority <= 1) {
        raft_become_leader_locked(node);
        return;
    }

    if (node->listen_fd < 0) {
        return;
    }

    csilk_raft_rv_req_t rv;
    memset(&rv, 0, sizeof(rv));
    rv.term = node->current_term;
    rv.candidate_id = node->config.node_id;
    rv.last_log_index = node->last_log_index;
    rv.last_log_term = _raft_log_term_at_locked(node, node->last_log_index);
    snprintf(rv.candidate_str, sizeof(rv.candidate_str), "%u", node->config.node_id);

    uint8_t pkt[sizeof(csilk_raft_hdr_t) + sizeof(csilk_raft_rv_req_t)];
    int     plen = _raft_rpc_encode_rv_req(
        &rv, pkt + sizeof(csilk_raft_hdr_t), sizeof(pkt) - sizeof(csilk_raft_hdr_t));
    if (plen < 0) {
        return;
    }
    if (csilk_raft_rpc_encode_hdr(
            CSILK_RAFT_MSG_REQUEST_VOTE_REQ, node->current_term, (uint32_t)plen, pkt, sizeof(pkt)) <
        0) {
        return;
    }

    for (size_t i = 0; i < node->peer_count; i++) {
        raft_send_packet_locked(
            node, &node->peers[i], pkt, sizeof(csilk_raft_hdr_t) + (size_t)plen);
    }
}

static int
raft_encode_reply(csilk_raft_msg_type_t msg_type,
                  uint64_t              term,
                  const void*           payload,
                  int                   payload_len,
                  uint8_t*              out_reply,
                  size_t                out_cap,
                  size_t*               out_reply_len)
{
    if (!out_reply || !out_reply_len || payload_len < 0) {
        return -1;
    }
    if (out_cap < sizeof(csilk_raft_hdr_t) + (size_t)payload_len) {
        return -1;
    }
    if (csilk_raft_rpc_encode_hdr(msg_type, term, (uint32_t)payload_len, out_reply, out_cap) < 0) {
        return -1;
    }
    if (payload_len > 0) {
        memcpy(out_reply + sizeof(csilk_raft_hdr_t), payload, (size_t)payload_len);
    }
    *out_reply_len = sizeof(csilk_raft_hdr_t) + (size_t)payload_len;
    return 0;
}

static void
raft_handle_rv_req_locked(csilk_raft_node_t* node,
                          const uint8_t*     payload,
                          size_t             payload_len,
                          uint8_t*           out_reply,
                          size_t             out_cap,
                          size_t*            out_reply_len)
{
    csilk_raft_rv_req_t req;
    if (_raft_rpc_decode_rv_req(payload, payload_len, &req) != 0) {
        return;
    }

    if (req.term > node->current_term) {
        raft_step_down_locked(node, req.term);
    }

    csilk_raft_rv_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.term = node->current_term;
    resp.voter_id = node->config.node_id;
    resp.vote_granted = 0;

    if (req.term >= node->current_term) {
        char cand_str[64];
        if (req.candidate_str[0] != '\0') {
            snprintf(cand_str, sizeof(cand_str), "%s", req.candidate_str);
        } else {
            snprintf(cand_str, sizeof(cand_str), "%u", req.candidate_id);
        }

        int      can_vote = (node->voted_for[0] == '\0' || strcmp(node->voted_for, cand_str) == 0);
        uint64_t my_last_term = _raft_log_term_at_locked(node, node->last_log_index);
        int      up_to_date =
            (req.last_log_term > my_last_term) ||
            (req.last_log_term == my_last_term && req.last_log_index >= node->last_log_index);

        if (can_vote && up_to_date) {
            snprintf(node->voted_for, sizeof(node->voted_for), "%s", cand_str);
            (void)_raft_wal_save_meta_locked(node);
            node->last_heartbeat_time = _csilk_raft_now_ms();
            resp.vote_granted = 1;
        }
    }

    uint8_t body[sizeof(csilk_raft_rv_resp_t)];
    int     blen = _raft_rpc_encode_rv_resp(&resp, body, sizeof(body));
    (void)raft_encode_reply(CSILK_RAFT_MSG_REQUEST_VOTE_RESP,
                            node->current_term,
                            body,
                            blen,
                            out_reply,
                            out_cap,
                            out_reply_len);
}

static void
raft_handle_rv_resp_locked(csilk_raft_node_t*        node,
                           const uint8_t*            payload,
                           size_t                    payload_len,
                           const struct sockaddr_in* src_addr)
{
    csilk_raft_rv_resp_t resp;
    if (_raft_rpc_decode_rv_resp(payload, payload_len, &resp) != 0) {
        return;
    }

    if (resp.term > node->current_term) {
        raft_step_down_locked(node, resp.term);
        return;
    }

    if (node->role != CSILK_RAFT_ROLE_CANDIDATE || resp.term != node->current_term ||
        !resp.vote_granted) {
        return;
    }

    csilk_raft_peer_t* peer = raft_find_peer_locked(node, resp.voter_id, src_addr);
    if (peer) {
        peer->vote_granted = 1;
    }

    size_t votes = 1;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].vote_granted) {
            votes++;
        }
    }

    if (votes >= raft_cluster_majority_locked(node)) {
        raft_become_leader_locked(node);
    }
}

static void
raft_handle_ae_req_locked(csilk_raft_node_t* node,
                          const uint8_t*     payload,
                          size_t             payload_len,
                          uint8_t*           out_reply,
                          size_t             out_cap,
                          size_t*            out_reply_len)
{
    csilk_raft_ae_req_t     req;
    csilk_raft_log_entry_t* entries = NULL;
    size_t                  entry_count = 0;
    if (_raft_rpc_decode_ae_req(payload, payload_len, &req, &entries, &entry_count) != 0) {
        return;
    }

    if (req.term > node->current_term) {
        raft_step_down_locked(node, req.term);
    }

    csilk_raft_ae_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.term = node->current_term;
    resp.follower_id = node->config.node_id;
    resp.success = 0;

    if (req.term < node->current_term) {
        goto send_reply;
    }

    if (node->role != CSILK_RAFT_ROLE_FOLLOWER) {
        node->role = CSILK_RAFT_ROLE_FOLLOWER;
    }
    node->last_heartbeat_time = _csilk_raft_now_ms();

    if (req.prev_log_index > node->last_log_index) {
        resp.conflict_index = node->last_log_index + 1ULL;
        resp.conflict_term = 0;
        goto send_reply;
    }

    if (req.prev_log_index >= node->log_base_index && req.prev_log_index > 0) {
        uint64_t local_prev_term = _raft_log_term_at_locked(node, req.prev_log_index);
        if (local_prev_term != req.prev_log_term) {
            uint64_t first_idx = req.prev_log_index;
            while (first_idx > node->log_base_index + 1ULL &&
                   _raft_log_term_at_locked(node, first_idx - 1ULL) == local_prev_term) {
                first_idx--;
            }
            resp.conflict_term = local_prev_term;
            resp.conflict_index = first_idx;
            goto send_reply;
        }
    }

    for (size_t i = 0; i < entry_count; i++) {
        uint64_t idx = entries[i].index;
        if (idx == 0) {
            idx = req.prev_log_index + 1ULL + (uint64_t)i;
        }
        if (idx <= node->log_base_index) {
            continue;
        }
        if (idx <= node->last_log_index) {
            uint64_t existing_term = _raft_log_term_at_locked(node, idx);
            if (existing_term == entries[i].term) {
                continue;
            }
            if (_raft_wal_truncate_from_locked(node, idx) != 0) {
                goto send_reply;
            }
        }
        if (idx != node->last_log_index + 1ULL) {
            goto send_reply;
        }
        if (_raft_wal_append_locked(node, entries[i].term, entries[i].data, entries[i].len, NULL) !=
            0) {
            goto send_reply;
        }
    }

    uint64_t match_up_to = req.prev_log_index + (uint64_t)entry_count;
    if (match_up_to < node->log_base_index) {
        match_up_to = node->log_base_index;
    }

    if (req.leader_commit > node->commit_index) {
        uint64_t new_commit = req.leader_commit < match_up_to ? req.leader_commit : match_up_to;
        if (new_commit > node->commit_index) {
            node->commit_index = new_commit;
            _raft_apply_committed_locked(node);
            csilk_cond_broadcast(&node->commit_cond);
        }
    }

    resp.success = 1;
    resp.match_index = match_up_to;

send_reply:
    for (size_t i = 0; i < entry_count; i++) {
        free(entries[i].data);
    }
    free(entries);

    uint8_t body[sizeof(csilk_raft_ae_resp_t)];
    int     blen = _raft_rpc_encode_ae_resp(&resp, body, sizeof(body));
    (void)raft_encode_reply(CSILK_RAFT_MSG_APPEND_ENTRIES_RESP,
                            node->current_term,
                            body,
                            blen,
                            out_reply,
                            out_cap,
                            out_reply_len);
}

static void
raft_handle_ae_resp_locked(csilk_raft_node_t*        node,
                           const uint8_t*            payload,
                           size_t                    payload_len,
                           const struct sockaddr_in* src_addr)
{
    csilk_raft_ae_resp_t resp;
    if (_raft_rpc_decode_ae_resp(payload, payload_len, &resp) != 0) {
        return;
    }

    if (resp.term > node->current_term) {
        raft_step_down_locked(node, resp.term);
        return;
    }

    if (node->role != CSILK_RAFT_ROLE_LEADER || resp.term != node->current_term) {
        return;
    }

    csilk_raft_peer_t* peer = raft_find_peer_locked(node, resp.follower_id, src_addr);
    if (!peer) {
        return;
    }

    if (resp.success) {
        if (resp.match_index > peer->match_index) {
            peer->match_index = resp.match_index;
        }
        peer->next_index = peer->match_index + 1ULL;
        raft_advance_leader_commit_locked(node);
    } else {
        if (resp.conflict_index > 0) {
            peer->next_index = resp.conflict_index;
        } else if (peer->next_index > 1) {
            peer->next_index--;
        }
        if (peer->next_index <= node->log_base_index) {
            peer->snapshot_offset = 0;
        }
    }
}

static void
raft_handle_is_req_locked(csilk_raft_node_t* node,
                          const uint8_t*     payload,
                          size_t             payload_len,
                          uint8_t*           out_reply,
                          size_t             out_cap,
                          size_t*            out_reply_len)
{
    csilk_raft_is_req_t req;
    const uint8_t*      chunk = NULL;
    if (_raft_rpc_decode_is_req(payload, payload_len, &req, &chunk) != 0) {
        return;
    }

    if (req.term > node->current_term) {
        raft_step_down_locked(node, req.term);
    }

    csilk_raft_is_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.term = node->current_term;
    resp.follower_id = node->config.node_id;
    resp.success = 0;

    if (req.term < node->current_term || req.total_len == 0 ||
        req.total_len > CSILK_RAFT_MAX_FRAME_DATA) {
        goto send_reply;
    }

    if (node->role != CSILK_RAFT_ROLE_FOLLOWER) {
        node->role = CSILK_RAFT_ROLE_FOLLOWER;
    }
    node->last_heartbeat_time = _csilk_raft_now_ms();

    if (req.last_included_index <= node->snapshot_last_index) {
        resp.success = 1;
        resp.next_offset = req.total_len;
        goto send_reply;
    }

    if (req.offset == 0) {
        free(node->snap_rx_buf);
        node->snap_rx_buf = malloc((size_t)req.total_len);
        if (!node->snap_rx_buf) {
            goto send_reply;
        }
        node->snap_rx_total = (size_t)req.total_len;
        node->snap_rx_received = 0;
        node->snap_rx_index = req.last_included_index;
        node->snap_rx_term = req.last_included_term;
    }

    if (!node->snap_rx_buf || req.last_included_index != node->snap_rx_index ||
        req.total_len != (uint64_t)node->snap_rx_total ||
        req.offset != (uint64_t)node->snap_rx_received ||
        req.offset + (uint64_t)req.chunk_len > req.total_len) {
        resp.next_offset = (uint64_t)node->snap_rx_received;
        goto send_reply;
    }

    if (req.chunk_len > 0 && chunk) {
        memcpy(node->snap_rx_buf + req.offset, chunk, req.chunk_len);
        node->snap_rx_received += req.chunk_len;
    }

    resp.success = 1;
    resp.next_offset = (uint64_t)node->snap_rx_received;

    if (req.done && node->snap_rx_received == node->snap_rx_total) {
        if (_raft_snapshot_install_locked(node,
                                          node->snap_rx_index,
                                          node->snap_rx_term,
                                          node->snap_rx_buf,
                                          node->snap_rx_total) != 0) {
            resp.success = 0;
        } else {
            node->last_applied = node->snap_rx_index;
            if (node->snapshot_apply_cb && node->snapshot_data) {
                node->snapshot_apply_cb(node->snapshot_last_index,
                                        node->snapshot_last_term,
                                        node->snapshot_data,
                                        node->snapshot_len,
                                        node->apply_user_data);
            }
            csilk_cond_broadcast(&node->commit_cond);
        }
        free(node->snap_rx_buf);
        node->snap_rx_buf = NULL;
        node->snap_rx_total = 0;
        node->snap_rx_received = 0;
    }

send_reply: {
    uint8_t body[sizeof(csilk_raft_is_resp_t)];
    int     blen = _raft_rpc_encode_is_resp(&resp, body, sizeof(body));
    (void)raft_encode_reply(CSILK_RAFT_MSG_INSTALL_SNAPSHOT_RESP,
                            node->current_term,
                            body,
                            blen,
                            out_reply,
                            out_cap,
                            out_reply_len);
}
}

static void
raft_handle_is_resp_locked(csilk_raft_node_t*        node,
                           const uint8_t*            payload,
                           size_t                    payload_len,
                           const struct sockaddr_in* src_addr)
{
    csilk_raft_is_resp_t resp;
    if (_raft_rpc_decode_is_resp(payload, payload_len, &resp) != 0) {
        return;
    }

    if (resp.term > node->current_term) {
        raft_step_down_locked(node, resp.term);
        return;
    }

    if (node->role != CSILK_RAFT_ROLE_LEADER || resp.term != node->current_term) {
        return;
    }

    csilk_raft_peer_t* peer = raft_find_peer_locked(node, resp.follower_id, src_addr);
    if (!peer) {
        return;
    }

    if (resp.success) {
        peer->snapshot_offset = resp.next_offset;
        if (peer->snapshot_offset >= node->snapshot_len) {
            peer->snapshot_offset = 0;
            if (node->snapshot_last_index > peer->match_index) {
                peer->match_index = node->snapshot_last_index;
            }
            peer->next_index = node->snapshot_last_index + 1ULL;
        }
    } else {
        peer->snapshot_offset = resp.next_offset;
    }
}

int
_raft_handle_rpc_packet(csilk_raft_node_t*        node,
                        const uint8_t*            pkt,
                        size_t                    pkt_len,
                        const struct sockaddr_in* src_addr,
                        uint8_t*                  out_reply,
                        size_t                    out_cap,
                        size_t*                   out_reply_len)
{
    if (!node || !pkt || pkt_len < sizeof(csilk_raft_hdr_t)) {
        return -1;
    }
    if (out_reply_len) {
        *out_reply_len = 0;
    }

    csilk_raft_hdr_t hdr;
    if (csilk_raft_rpc_decode_hdr(pkt, pkt_len, &hdr) != 0) {
        return -1;
    }
    if ((size_t)hdr.payload_len > pkt_len - sizeof(csilk_raft_hdr_t)) {
        return -1;
    }

    const uint8_t* payload = pkt + sizeof(csilk_raft_hdr_t);
    size_t         payload_len = (size_t)hdr.payload_len;
    size_t         local_reply_len = 0;

    csilk_mutex_lock(&node->mutex);
    switch ((csilk_raft_msg_type_t)hdr.msg_type) {
    case CSILK_RAFT_MSG_REQUEST_VOTE_REQ:
        raft_handle_rv_req_locked(node, payload, payload_len, out_reply, out_cap, &local_reply_len);
        break;
    case CSILK_RAFT_MSG_REQUEST_VOTE_RESP:
        raft_handle_rv_resp_locked(node, payload, payload_len, src_addr);
        break;
    case CSILK_RAFT_MSG_APPEND_ENTRIES_REQ:
        raft_handle_ae_req_locked(node, payload, payload_len, out_reply, out_cap, &local_reply_len);
        break;
    case CSILK_RAFT_MSG_APPEND_ENTRIES_RESP:
        raft_handle_ae_resp_locked(node, payload, payload_len, src_addr);
        break;
    case CSILK_RAFT_MSG_INSTALL_SNAPSHOT_REQ:
        raft_handle_is_req_locked(node, payload, payload_len, out_reply, out_cap, &local_reply_len);
        break;
    case CSILK_RAFT_MSG_INSTALL_SNAPSHOT_RESP:
        raft_handle_is_resp_locked(node, payload, payload_len, src_addr);
        break;
    default:
        break;
    }
    csilk_mutex_unlock(&node->mutex);

    if (out_reply_len) {
        *out_reply_len = local_reply_len;
    }
    return 0;
}

static void
raft_transport_loop(void* arg)
{
    csilk_raft_node_t* node = (csilk_raft_node_t*)arg;
    uint8_t*           rx_buf = malloc(CSILK_RAFT_RPC_MAX_PACKET);
    uint8_t*           tx_buf = malloc(CSILK_RAFT_RPC_MAX_PACKET);
    if (!rx_buf || !tx_buf) {
        free(rx_buf);
        free(tx_buf);
        return;
    }

    for (;;) {
        csilk_mutex_lock(&node->mutex);
        int stop = node->transport_stop;
        int fd = node->listen_fd;
        csilk_mutex_unlock(&node->mutex);

        if (stop) {
            break;
        }

        if (fd >= 0) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc = poll(&pfd, 1, 10);
            if (prc > 0 && (pfd.revents & POLLIN)) {
                for (;;) {
                    struct sockaddr_in src_addr = {0};
                    socklen_t          slen = sizeof(src_addr);
                    ssize_t            n = recvfrom(fd,
                                         rx_buf,
                                         CSILK_RAFT_RPC_MAX_PACKET,
                                         0,
                                         (struct sockaddr*)&src_addr,
                                         &slen);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                            break;
                        }
                        break;
                    }
                    if (n > 0) {
                        size_t reply_len = 0;
                        if (_raft_handle_rpc_packet(node,
                                                    rx_buf,
                                                    (size_t)n,
                                                    &src_addr,
                                                    tx_buf,
                                                    CSILK_RAFT_RPC_MAX_PACKET,
                                                    &reply_len) == 0 &&
                            reply_len > 0) {
                            (void)sendto(
                                fd, tx_buf, reply_len, 0, (const struct sockaddr*)&src_addr, slen);
                        }
                    }
                }
            }
        } else {
            struct timespec req = {0, 10000000L};
            nanosleep(&req, NULL);
        }

        (void)csilk_raft_node_tick(node, 0);
    }

    free(rx_buf);
    free(tx_buf);
}

int
csilk_raft_node_add_peer(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr)
{
    if (!node || !peer_id || !peer_addr) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    for (size_t i = 0; i < node->peer_count; i++) {
        if (strcmp(node->peers[i].peer_id, peer_id) == 0) {
            snprintf(node->peers[i].peer_addr, sizeof(node->peers[i].peer_addr), "%s", peer_addr);
            node->peers[i].addr_resolved = 0;
            csilk_mutex_unlock(&node->mutex);
            return 0;
        }
    }

    if (node->peer_count >= CSILK_RAFT_MAX_PEERS) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    csilk_raft_peer_t* peer = &node->peers[node->peer_count++];
    memset(peer, 0, sizeof(*peer));
    snprintf(peer->peer_id, sizeof(peer->peer_id), "%s", peer_id);
    snprintf(peer->peer_addr, sizeof(peer->peer_addr), "%s", peer_addr);
    peer->next_index = node->last_log_index + 1ULL;
    peer->match_index = 0;

    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_node_listen(csilk_raft_node_t* node, const char* bind_addr)
{
    if (!node || !bind_addr) {
        return -1;
    }

    char     host[128];
    uint16_t port = 0;
    if (raft_parse_host_port(bind_addr, host, sizeof(host), &port) != 0) {
        return -1;
    }

    struct sockaddr_in sin = {0};
    if (raft_resolve_ipv4(host, port, 1, &sin) != 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    if (bind(fd, (const struct sockaddr*)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in bound = {0};
    socklen_t          blen = sizeof(bound);
    if (getsockname(fd, (struct sockaddr*)&bound, &blen) != 0) {
        close(fd);
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    if (node->listen_fd >= 0) {
        close(node->listen_fd);
    }
    node->listen_fd = fd;
    node->listen_port = ntohs(bound.sin_port);
    snprintf(node->listen_addr, sizeof(node->listen_addr), "%s", bind_addr);
    csilk_mutex_unlock(&node->mutex);
    return 0;
}

uint16_t
csilk_raft_node_get_port(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint16_t port = node->listen_port;
    csilk_mutex_unlock(&mutable_node->mutex);
    return port;
}

int
csilk_raft_node_start(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    uint64_t now = _csilk_raft_now_ms();
    node->started = 1;
    node->last_heartbeat_time = now;
    node->election_timeout_ms = raft_pick_timeout_locked(node);

    if (node->peer_count == 0) {
        node->role = CSILK_RAFT_ROLE_LEADER;
        node->current_term++;
        (void)_raft_wal_save_meta_locked(node);
    } else if (node->listen_fd >= 0 && !node->transport_running) {
        node->transport_stop = 0;
        if (csilk_thread_create(&node->transport_thread, raft_transport_loop, node) == 0) {
            node->transport_running = 1;
        } else {
            csilk_mutex_unlock(&node->mutex);
            return -1;
        }
    }
    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_node_stop(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    int            was_running = node->transport_running;
    csilk_thread_t tid = node->transport_thread;
    node->transport_stop = 1;
    node->started = 0;
    csilk_cond_broadcast(&node->commit_cond);
    csilk_mutex_unlock(&node->mutex);

    if (was_running) {
        (void)csilk_thread_join(&tid);
    }

    csilk_mutex_lock(&node->mutex);
    node->transport_running = 0;
    if (node->listen_fd >= 0) {
        close(node->listen_fd);
        node->listen_fd = -1;
        node->listen_port = 0;
    }
    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_node_tick(csilk_raft_node_t* node, uint64_t now_ms)
{
    if (!node) {
        return -1;
    }

    uint64_t now = (now_ms > 0) ? now_ms : _csilk_raft_now_ms();

    csilk_mutex_lock(&node->mutex);
    if (node->config.sync_policy == CSILK_RAFT_SYNC_INTERVAL_MS && node->pending_sync_entries > 0) {
        if (now >= node->last_sync_ms &&
            (now - node->last_sync_ms) >= (uint64_t)node->config.sync_interval_ms) {
            (void)_raft_wal_flush_locked(node);
        }
    }

    if (node->role == CSILK_RAFT_ROLE_LEADER) {
        if (now >= node->last_broadcast_time &&
            (now - node->last_broadcast_time) >= node->config.heartbeat_interval_ms) {
            _raft_broadcast_append_entries_locked(node);
        }
    } else if ((node->started || now_ms > 0) && node->peer_count > 0) {
        if (now >= node->last_heartbeat_time &&
            (now - node->last_heartbeat_time) >= node->election_timeout_ms) {
            raft_start_election_locked(node, now);
        }
    }
    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_replicate_wait(csilk_raft_node_t* node,
                          const uint8_t*     data,
                          size_t             len,
                          uint32_t           timeout_ms,
                          uint64_t*          out_index)
{
    if (!node || !data || len == 0) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    if (node->role != CSILK_RAFT_ROLE_LEADER) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    uint64_t target_idx = 0;
    if (_raft_wal_append_locked(node, node->current_term, data, len, &target_idx) != 0) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    size_t majority = raft_cluster_majority_locked(node);
    if (majority <= 1) {
        if (node->commit_index < target_idx) {
            node->commit_index = target_idx;
            _raft_apply_committed_locked(node);
            csilk_cond_broadcast(&node->commit_cond);
        }
        if (out_index) {
            *out_index = target_idx;
        }
        csilk_mutex_unlock(&node->mutex);
        return 0;
    }

    _raft_broadcast_append_entries_locked(node);

    uint64_t deadline = _csilk_raft_now_ms() + (uint64_t)timeout_ms;
    while (node->commit_index < target_idx && node->role == CSILK_RAFT_ROLE_LEADER &&
           !node->transport_stop) {
        uint64_t now = _csilk_raft_now_ms();
        if (now >= deadline) {
            break;
        }
        uint64_t remain_ms = deadline - now;
        uint64_t slice_ms = remain_ms > 20ULL ? 20ULL : remain_ms;
        (void)csilk_cond_timedwait(&node->commit_cond, &node->mutex, slice_ms * 1000000ULL);
        if (node->commit_index < target_idx && node->role == CSILK_RAFT_ROLE_LEADER &&
            _csilk_raft_now_ms() - node->last_broadcast_time >=
                node->config.heartbeat_interval_ms) {
            _raft_broadcast_append_entries_locked(node);
        }
    }

    int ok = (node->commit_index >= target_idx) ? 0 : -1;
    if (ok == 0 && out_index) {
        *out_index = target_idx;
    }
    csilk_mutex_unlock(&node->mutex);
    return ok;
}

int
csilk_raft_set_apply_cb(csilk_raft_node_t*             node,
                        csilk_raft_apply_cb_t          on_entry,
                        csilk_raft_snapshot_apply_cb_t on_snapshot,
                        void*                          user_data)
{
    if (!node) {
        return -1;
    }
    csilk_mutex_lock(&node->mutex);
    node->apply_cb = on_entry;
    node->snapshot_apply_cb = on_snapshot;
    node->apply_user_data = user_data;
    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_replay_committed(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }
    csilk_mutex_lock(&node->mutex);
    if (node->snapshot_data && node->snapshot_len > 0 &&
        node->last_applied < node->snapshot_last_index) {
        node->last_applied = node->snapshot_last_index;
        if (node->snapshot_apply_cb) {
            node->snapshot_apply_cb(node->snapshot_last_index,
                                    node->snapshot_last_term,
                                    node->snapshot_data,
                                    node->snapshot_len,
                                    node->apply_user_data);
        }
    }
    _raft_apply_committed_locked(node);
    csilk_mutex_unlock(&node->mutex);
    return 0;
}
