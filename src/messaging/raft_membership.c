/**
 * @file raft_membership.c
 * @brief Raft Joint Consensus dynamic cluster membership transitions (C_old,new -> C_new).
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

/* --- Internal Helpers --- */

static const csilk_raft_node_info_t*
_raft_find_node_info(const csilk_raft_node_info_t* nodes, size_t count, const char* id)
{
    if (!nodes || !id) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(nodes[i].id, id) == 0) {
            return &nodes[i];
        }
    }
    return NULL;
}

static int
_raft_set_has_ack_majority(const csilk_raft_node_t*      node,
                           const csilk_raft_node_info_t* nodes,
                           size_t                        count,
                           uint64_t                      index)
{
    if (count == 0) {
        return 1;
    }

    size_t majority = (count / 2U) + 1U;
    size_t acks = 0;
    char   local_id[64];
    snprintf(local_id, sizeof(local_id), "%u", node->config.node_id);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(nodes[i].id, local_id) == 0) {
            acks++;
            continue;
        }
        for (size_t p = 0; p < node->peer_count; p++) {
            if (strcmp(node->peers[p].peer_id, nodes[i].id) == 0) {
                if (node->peers[p].match_index >= index) {
                    acks++;
                }
                break;
            }
        }
    }
    return (acks >= majority) ? 1 : 0;
}

static int
_raft_set_has_vote_majority(const csilk_raft_node_t*      node,
                            const csilk_raft_node_info_t* nodes,
                            size_t                        count)
{
    if (count == 0) {
        return 1;
    }

    size_t majority = (count / 2U) + 1U;
    size_t votes = 0;
    char   local_id[64];
    snprintf(local_id, sizeof(local_id), "%u", node->config.node_id);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(nodes[i].id, local_id) == 0) {
            votes++;
            continue;
        }
        for (size_t p = 0; p < node->peer_count; p++) {
            if (strcmp(node->peers[p].peer_id, nodes[i].id) == 0) {
                if (node->peers[p].vote_granted) {
                    votes++;
                }
                break;
            }
        }
    }
    return (votes >= majority) ? 1 : 0;
}

static void
_raft_ensure_peer_exists_locked(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr)
{
    char local_id[64];
    snprintf(local_id, sizeof(local_id), "%u", node->config.node_id);
    if (strcmp(peer_id, local_id) == 0) {
        return;
    }

    for (size_t i = 0; i < node->peer_count; i++) {
        if (strcmp(node->peers[i].peer_id, peer_id) == 0) {
            if (peer_addr && peer_addr[0] != '\0') {
                snprintf(
                    node->peers[i].peer_addr, sizeof(node->peers[i].peer_addr), "%s", peer_addr);
                node->peers[i].addr_resolved = 0;
            }
            return;
        }
    }

    if (node->peer_count >= CSILK_RAFT_MAX_PEERS) {
        return;
    }

    csilk_raft_peer_t* peer = &node->peers[node->peer_count++];
    memset(peer, 0, sizeof(*peer));
    snprintf(peer->peer_id, sizeof(peer->peer_id), "%s", peer_id);
    if (peer_addr) {
        snprintf(peer->peer_addr, sizeof(peer->peer_addr), "%s", peer_addr);
    }
    peer->next_index = node->last_log_index + 1ULL;
    peer->match_index = 0;
}

/* --- Internal Membership Interface --- */

void
_raft_membership_init_locked(csilk_raft_node_t* node)
{
    if (!node) {
        return;
    }

    memset(&node->membership, 0, sizeof(node->membership));
    node->membership.state = CSILK_RAFT_CONF_STABLE;

    /* Populate local node info */
    snprintf(node->membership.old_nodes[0].id,
             sizeof(node->membership.old_nodes[0].id),
             "%u",
             node->config.node_id);
    if (node->listen_addr[0] != '\0') {
        snprintf(node->membership.old_nodes[0].addr,
                 sizeof(node->membership.old_nodes[0].addr),
                 "%s",
                 node->listen_addr);
    }
    node->membership.old_count = 1;

    /* Add pre-configured peers */
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->membership.old_count < CSILK_RAFT_MAX_MEMBERS) {
            snprintf(node->membership.old_nodes[node->membership.old_count].id,
                     sizeof(node->membership.old_nodes[node->membership.old_count].id),
                     "%s",
                     node->peers[i].peer_id);
            snprintf(node->membership.old_nodes[node->membership.old_count].addr,
                     sizeof(node->membership.old_nodes[node->membership.old_count].addr),
                     "%s",
                     node->peers[i].peer_addr);
            node->membership.old_count++;
        }
    }
}

int
_raft_membership_is_log_committed_locked(const csilk_raft_node_t* node, uint64_t index)
{
    if (!node) {
        return 0;
    }

    if (node->membership.state == CSILK_RAFT_CONF_JOINT) {
        /* Dual majority required: majority of C_old AND majority of C_new */
        int old_ok = _raft_set_has_ack_majority(
            node, node->membership.old_nodes, node->membership.old_count, index);
        int new_ok = _raft_set_has_ack_majority(
            node, node->membership.new_nodes, node->membership.new_count, index);
        return (old_ok && new_ok) ? 1 : 0;
    }

    /* Single majority for stable configuration */
    if (node->membership.old_count > 0) {
        return _raft_set_has_ack_majority(
            node, node->membership.old_nodes, node->membership.old_count, index);
    }

    /* Fallback if uninitialized */
    size_t total = node->peer_count + 1U;
    size_t majority = (total / 2U) + 1U;
    size_t acks = 1;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].match_index >= index) {
            acks++;
        }
    }
    return (acks >= majority) ? 1 : 0;
}

int
_raft_membership_is_election_won_locked(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }

    if (node->membership.state == CSILK_RAFT_CONF_JOINT) {
        /* Dual majority of votes required */
        int old_ok = _raft_set_has_vote_majority(
            node, node->membership.old_nodes, node->membership.old_count);
        int new_ok = _raft_set_has_vote_majority(
            node, node->membership.new_nodes, node->membership.new_count);
        return (old_ok && new_ok) ? 1 : 0;
    }

    if (node->membership.old_count > 0) {
        return _raft_set_has_vote_majority(
            node, node->membership.old_nodes, node->membership.old_count);
    }

    size_t total = node->peer_count + 1U;
    size_t majority = (total / 2U) + 1U;
    size_t votes = 1;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].vote_granted) {
            votes++;
        }
    }
    return (votes >= majority) ? 1 : 0;
}

int
_raft_membership_apply_entry_locked(csilk_raft_node_t* node,
                                    uint64_t           index,
                                    const uint8_t*     data,
                                    size_t             len)
{
    if (!node || !data || len < sizeof(csilk_raft_conf_entry_hdr_t)) {
        return 0;
    }

    const csilk_raft_conf_entry_hdr_t* hdr = (const csilk_raft_conf_entry_hdr_t*)data;
    if (hdr->magic != CSILK_RAFT_CONF_MAGIC) {
        return 0;
    }

    if (hdr->op == CSILK_RAFT_CONF_OP_JOINT) {
        size_t expected_len =
            sizeof(csilk_raft_conf_entry_hdr_t) +
            (size_t)(hdr->old_count + hdr->new_count) * sizeof(csilk_raft_node_info_t);
        if (len < expected_len || hdr->old_count > CSILK_RAFT_MAX_MEMBERS ||
            hdr->new_count > CSILK_RAFT_MAX_MEMBERS) {
            return -1;
        }

        const csilk_raft_node_info_t* old_src =
            (const csilk_raft_node_info_t*)(data + sizeof(csilk_raft_conf_entry_hdr_t));
        const csilk_raft_node_info_t* new_src = old_src + hdr->old_count;

        node->membership.state = CSILK_RAFT_CONF_JOINT;
        node->membership.joint_index = index;
        node->membership.new_index = 0;
        node->membership.old_count = hdr->old_count;
        node->membership.new_count = hdr->new_count;

        memcpy(
            node->membership.old_nodes, old_src, hdr->old_count * sizeof(csilk_raft_node_info_t));
        memcpy(
            node->membership.new_nodes, new_src, hdr->new_count * sizeof(csilk_raft_node_info_t));

        /* Add any newly introduced nodes to peer list */
        char joined_ids[CSILK_RAFT_MAX_PEERS][64];
        char joined_addrs[CSILK_RAFT_MAX_PEERS][128];
        size_t joined_count = 0;

        for (size_t i = 0; i < hdr->new_count; i++) {
            size_t old_peers = node->peer_count;
            _raft_ensure_peer_exists_locked(node, new_src[i].id, new_src[i].addr);
            if (node->peer_count > old_peers && joined_count < CSILK_RAFT_MAX_PEERS) {
                snprintf(joined_ids[joined_count], sizeof(joined_ids[0]), "%s", new_src[i].id);
                snprintf(joined_addrs[joined_count], sizeof(joined_addrs[0]), "%s", new_src[i].addr);
                joined_count++;
            }
        }
        if (node->topology_cb && joined_count > 0) {
            csilk_raft_topology_cb_t top_cb = node->topology_cb;
            void* top_ud = node->topology_user_data;
            for (size_t i = 0; i < joined_count; i++) {
                top_cb(node, CSILK_RAFT_TOPOLOGY_NODE_JOINED, joined_ids[i], joined_addrs[i], top_ud);
            }
        }
        return 1;
    }

    if (hdr->op == CSILK_RAFT_CONF_OP_FINAL) {
        size_t expected_len = sizeof(csilk_raft_conf_entry_hdr_t) +
                              (size_t)hdr->new_count * sizeof(csilk_raft_node_info_t);
        if (len < expected_len || hdr->new_count > CSILK_RAFT_MAX_MEMBERS) {
            return -1;
        }

        const csilk_raft_node_info_t* new_src =
            (const csilk_raft_node_info_t*)(data + sizeof(csilk_raft_conf_entry_hdr_t));

        node->membership.new_index = index;
        node->membership.new_count = hdr->new_count;
        memcpy(
            node->membership.new_nodes, new_src, hdr->new_count * sizeof(csilk_raft_node_info_t));

        for (size_t i = 0; i < hdr->new_count; i++) {
            _raft_ensure_peer_exists_locked(node, new_src[i].id, new_src[i].addr);
        }
        return 1;
    }

    return 0;
}

void
_raft_membership_on_commit_locked(csilk_raft_node_t* node)
{
    if (!node || node->membership.state != CSILK_RAFT_CONF_JOINT) {
        return;
    }

    /* Phase 1 committed: Leader proposes C_new entry (Phase 2) */
    if (node->role == CSILK_RAFT_ROLE_LEADER && node->membership.new_index == 0 &&
        node->commit_index >= node->membership.joint_index) {
        size_t total_len = sizeof(csilk_raft_conf_entry_hdr_t) +
                           node->membership.new_count * sizeof(csilk_raft_node_info_t);
        uint8_t* buf = calloc(1, total_len);
        if (buf) {
            csilk_raft_conf_entry_hdr_t* hdr = (csilk_raft_conf_entry_hdr_t*)buf;
            hdr->magic = CSILK_RAFT_CONF_MAGIC;
            hdr->op = CSILK_RAFT_CONF_OP_FINAL;
            hdr->old_count = 0;
            hdr->new_count = (uint16_t)node->membership.new_count;
            memcpy(buf + sizeof(csilk_raft_conf_entry_hdr_t),
                   node->membership.new_nodes,
                   node->membership.new_count * sizeof(csilk_raft_node_info_t));

            uint64_t new_idx = 0;
            if (_raft_wal_append_locked(node, node->current_term, buf, total_len, &new_idx) == 0) {
                node->membership.new_index = new_idx;
                _raft_broadcast_append_entries_locked(node);

                /* If Phase 2 entry already meets quorum (e.g. single node or majority satisfied), commit */
                if (_raft_membership_is_log_committed_locked(node, new_idx)) {
                    node->commit_index = new_idx;
                    _raft_apply_committed_locked(node);
                }
            }
            free(buf);
        }
    }

    /* Phase 2 committed: Finalize transition back to STABLE */
    if (node->membership.new_index > 0 && node->commit_index >= node->membership.new_index) {
        memcpy(node->membership.old_nodes,
               node->membership.new_nodes,
               sizeof(csilk_raft_node_info_t) * node->membership.new_count);
        node->membership.old_count = node->membership.new_count;
        node->membership.new_count = 0;
        node->membership.joint_index = 0;
        node->membership.new_index = 0;
        node->membership.state = CSILK_RAFT_CONF_STABLE;

        /* Prune peers that are no longer part of active cluster configuration */
        char   pruned_ids[CSILK_RAFT_MAX_PEERS][64];
        size_t pruned_count = 0;

        for (size_t p = 0; p < node->peer_count;) {
            int found = 0;
            for (size_t m = 0; m < node->membership.old_count; m++) {
                if (strcmp(node->peers[p].peer_id, node->membership.old_nodes[m].id) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (pruned_count < CSILK_RAFT_MAX_PEERS) {
                    snprintf(pruned_ids[pruned_count++],
                             sizeof(pruned_ids[0]),
                             "%s",
                             node->peers[p].peer_id);
                }
                for (size_t k = p; k + 1 < node->peer_count; k++) {
                    node->peers[k] = node->peers[k + 1];
                }
                node->peer_count--;
            } else {
                p++;
            }
        }

        /* Check if local node itself was removed from the cluster */
        char local_id[64];
        snprintf(local_id, sizeof(local_id), "%u", node->config.node_id);
        int local_found = 0;
        for (size_t m = 0; m < node->membership.old_count; m++) {
            if (strcmp(node->membership.old_nodes[m].id, local_id) == 0) {
                local_found = 1;
                break;
            }
        }
        if (!local_found && node->role == CSILK_RAFT_ROLE_LEADER) {
            node->role = CSILK_RAFT_ROLE_FOLLOWER;
        }

        csilk_cond_broadcast(&node->commit_cond);

        /* Notify topology listeners of pruned peers and configuration stabilization */
        csilk_raft_topology_cb_t top_cb = node->topology_cb;
        void*                    top_ud = node->topology_user_data;
        if (top_cb) {
            for (size_t i = 0; i < pruned_count; i++) {
                top_cb(node, CSILK_RAFT_TOPOLOGY_NODE_LEFT, pruned_ids[i], NULL, top_ud);
            }
            top_cb(node, CSILK_RAFT_TOPOLOGY_CONF_STABLE, NULL, NULL, top_ud);
        }
    }
}

void
_raft_membership_on_truncate_locked(csilk_raft_node_t* node, uint64_t from_index)
{
    if (!node || node->membership.state != CSILK_RAFT_CONF_JOINT) {
        return;
    }

    if (node->membership.new_index >= from_index) {
        node->membership.new_index = 0;
    }
    if (node->membership.joint_index >= from_index) {
        node->membership.state = CSILK_RAFT_CONF_STABLE;
        node->membership.joint_index = 0;
        node->membership.new_index = 0;
        node->membership.new_count = 0;
    }
}

/* --- Public API --- */

int
csilk_raft_add_node(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr)
{
    if (!node || !peer_id || peer_id[0] == '\0' || !peer_addr || peer_addr[0] == '\0') {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);

    if (node->role != CSILK_RAFT_ROLE_LEADER) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    if (node->membership.state != CSILK_RAFT_CONF_STABLE) {
        csilk_mutex_unlock(&node->mutex);
        return -1; /* Reconfiguration already in progress */
    }

    if (node->membership.old_count >= CSILK_RAFT_MAX_MEMBERS) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    char local_id[64];
    snprintf(local_id, sizeof(local_id), "%u", node->config.node_id);
    if (strcmp(peer_id, local_id) == 0) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    if (_raft_find_node_info(node->membership.old_nodes, node->membership.old_count, peer_id)) {
        csilk_mutex_unlock(&node->mutex);
        return -1; /* Already a member */
    }

    /* Prepare new_nodes = old_nodes + (peer_id, peer_addr) */
    csilk_raft_node_info_t new_nodes[CSILK_RAFT_MAX_MEMBERS];
    memcpy(new_nodes,
           node->membership.old_nodes,
           node->membership.old_count * sizeof(csilk_raft_node_info_t));
    size_t new_count = node->membership.old_count;

    snprintf(new_nodes[new_count].id, sizeof(new_nodes[new_count].id), "%s", peer_id);
    snprintf(new_nodes[new_count].addr, sizeof(new_nodes[new_count].addr), "%s", peer_addr);
    new_count++;

    /* Register peer in transport table so replication traffic starts */
    _raft_ensure_peer_exists_locked(node, peer_id, peer_addr);

    /* Construct Joint Consensus C_old,new log entry */
    size_t total_len = sizeof(csilk_raft_conf_entry_hdr_t) +
                       (node->membership.old_count + new_count) * sizeof(csilk_raft_node_info_t);
    uint8_t* buf = calloc(1, total_len);
    if (!buf) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    csilk_raft_conf_entry_hdr_t* hdr = (csilk_raft_conf_entry_hdr_t*)buf;
    hdr->magic = CSILK_RAFT_CONF_MAGIC;
    hdr->op = CSILK_RAFT_CONF_OP_JOINT;
    hdr->old_count = (uint16_t)node->membership.old_count;
    hdr->new_count = (uint16_t)new_count;

    uint8_t* p = buf + sizeof(csilk_raft_conf_entry_hdr_t);
    memcpy(
        p, node->membership.old_nodes, node->membership.old_count * sizeof(csilk_raft_node_info_t));
    p += node->membership.old_count * sizeof(csilk_raft_node_info_t);
    memcpy(p, new_nodes, new_count * sizeof(csilk_raft_node_info_t));

    uint64_t joint_idx = 0;
    if (_raft_wal_append_locked(node, node->current_term, buf, total_len, &joint_idx) != 0) {
        free(buf);
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }
    free(buf);

    node->membership.state = CSILK_RAFT_CONF_JOINT;
    node->membership.joint_index = joint_idx;
    node->membership.new_index = 0;
    node->membership.new_count = new_count;
    memcpy(node->membership.new_nodes, new_nodes, new_count * sizeof(csilk_raft_node_info_t));

    _raft_broadcast_append_entries_locked(node);

    if (_raft_membership_is_log_committed_locked(node, joint_idx)) {
        node->commit_index = joint_idx;
        _raft_apply_committed_locked(node);
        _raft_membership_on_commit_locked(node);
        csilk_cond_broadcast(&node->commit_cond);
    }

    csilk_raft_topology_cb_t top_cb = node->topology_cb;
    void*                    top_ud = node->topology_user_data;
    csilk_mutex_unlock(&node->mutex);
    if (top_cb) {
        top_cb(node, CSILK_RAFT_TOPOLOGY_NODE_JOINED, peer_id, peer_addr, top_ud);
    }
    return 0;
}

int
csilk_raft_remove_node(csilk_raft_node_t* node, const char* peer_id)
{
    if (!node || !peer_id || peer_id[0] == '\0') {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);

    if (node->role != CSILK_RAFT_ROLE_LEADER) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    if (node->membership.state != CSILK_RAFT_CONF_STABLE) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    if (node->membership.old_count <= 1) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    if (!_raft_find_node_info(node->membership.old_nodes, node->membership.old_count, peer_id)) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    /* Prepare new_nodes = old_nodes - {peer_id} */
    csilk_raft_node_info_t new_nodes[CSILK_RAFT_MAX_MEMBERS];
    size_t                 new_count = 0;
    for (size_t i = 0; i < node->membership.old_count; i++) {
        if (strcmp(node->membership.old_nodes[i].id, peer_id) != 0) {
            new_nodes[new_count++] = node->membership.old_nodes[i];
        }
    }

    /* Construct Joint Consensus C_old,new log entry */
    size_t total_len = sizeof(csilk_raft_conf_entry_hdr_t) +
                       (node->membership.old_count + new_count) * sizeof(csilk_raft_node_info_t);
    uint8_t* buf = calloc(1, total_len);
    if (!buf) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }

    csilk_raft_conf_entry_hdr_t* hdr = (csilk_raft_conf_entry_hdr_t*)buf;
    hdr->magic = CSILK_RAFT_CONF_MAGIC;
    hdr->op = CSILK_RAFT_CONF_OP_JOINT;
    hdr->old_count = (uint16_t)node->membership.old_count;
    hdr->new_count = (uint16_t)new_count;

    uint8_t* p = buf + sizeof(csilk_raft_conf_entry_hdr_t);
    memcpy(
        p, node->membership.old_nodes, node->membership.old_count * sizeof(csilk_raft_node_info_t));
    p += node->membership.old_count * sizeof(csilk_raft_node_info_t);
    memcpy(p, new_nodes, new_count * sizeof(csilk_raft_node_info_t));

    uint64_t joint_idx = 0;
    if (_raft_wal_append_locked(node, node->current_term, buf, total_len, &joint_idx) != 0) {
        free(buf);
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }
    free(buf);

    node->membership.state = CSILK_RAFT_CONF_JOINT;
    node->membership.joint_index = joint_idx;
    node->membership.new_index = 0;
    node->membership.new_count = new_count;
    memcpy(node->membership.new_nodes, new_nodes, new_count * sizeof(csilk_raft_node_info_t));

    _raft_broadcast_append_entries_locked(node);

    if (_raft_membership_is_log_committed_locked(node, joint_idx)) {
        node->commit_index = joint_idx;
        _raft_apply_committed_locked(node);
        _raft_membership_on_commit_locked(node);
        csilk_cond_broadcast(&node->commit_cond);
    }

    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_change_membership_wait(csilk_raft_node_t* node,
                                  const char*        peer_id,
                                  const char*        peer_addr,
                                  int                is_add,
                                  uint32_t           timeout_ms)
{
    if (!node || !peer_id) {
        return -1;
    }

    int rc = is_add ? csilk_raft_add_node(node, peer_id, peer_addr)
                    : csilk_raft_remove_node(node, peer_id);
    if (rc != 0) {
        return rc;
    }

    csilk_mutex_lock(&node->mutex);
    uint64_t deadline = _csilk_raft_now_ms() + (uint64_t)timeout_ms;

    while (node->membership.state == CSILK_RAFT_CONF_JOINT && !node->transport_stop) {
        uint64_t now = _csilk_raft_now_ms();
        if (now >= deadline) {
            csilk_mutex_unlock(&node->mutex);
            return -1;
        }

        uint64_t remain_ms = deadline - now;
        uint64_t slice_ms = remain_ms > 20ULL ? 20ULL : remain_ms;
        (void)csilk_cond_timedwait(&node->commit_cond, &node->mutex, slice_ms * 1000000ULL);

        if (node->role == CSILK_RAFT_ROLE_LEADER &&
            _csilk_raft_now_ms() - node->last_broadcast_time >=
                node->config.heartbeat_interval_ms) {
            _raft_broadcast_append_entries_locked(node);
        }
    }

    int res = (node->membership.state == CSILK_RAFT_CONF_STABLE) ? 0 : -1;
    csilk_mutex_unlock(&node->mutex);
    return res;
}

int
csilk_raft_get_membership(csilk_raft_node_t* node, csilk_raft_membership_t* out_membership)
{
    if (!node || !out_membership) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    memcpy(out_membership, &node->membership, sizeof(csilk_raft_membership_t));
    csilk_mutex_unlock(&node->mutex);
    return 0;
}

int
csilk_raft_set_topology_cb(csilk_raft_node_t* node, csilk_raft_topology_cb_t cb, void* user_data)
{
    if (!node) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    node->topology_cb = cb;
    node->topology_user_data = user_data;
    csilk_mutex_unlock(&node->mutex);
    return 0;
}
