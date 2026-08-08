#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

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
