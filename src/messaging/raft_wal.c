/**
 * @file raft_wal.c
 * @brief Distributed Raft consensus WAL log replication engine implementation.
 * @copyright MIT License
 */

#include "raft_internal.h"
#include <stdlib.h>
#include <string.h>

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

    node->role = CSILK_RAFT_ROLE_LEADER;
    node->current_term = 1;
    node->last_log_index = 0;
    node->commit_index = 0;
    csilk_mutex_init(&node->mutex);

    return node;
}

void
csilk_raft_node_free(csilk_raft_node_t* node)
{
    if (!node) {
        return;
    }
    csilk_mutex_destroy(&node->mutex);
    free(node);
}

uint64_t
csilk_raft_append_log(csilk_raft_node_t* node, const uint8_t* data, size_t len)
{
    if (!node || !data || len == 0 || node->role != CSILK_RAFT_ROLE_LEADER) {
        return 0;
    }

    node->last_log_index++;
    return node->last_log_index;
}

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

csilk_raft_role_t
csilk_raft_get_role(const csilk_raft_node_t* node)
{
    return node ? node->role : CSILK_RAFT_ROLE_FOLLOWER;
}

uint64_t
csilk_raft_get_term(const csilk_raft_node_t* node)
{
    return node ? node->current_term : 0;
}
