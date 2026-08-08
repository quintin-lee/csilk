#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

int
csilk_raft_snapshot_create(csilk_raft_node_t* node, const uint8_t* state_data, size_t state_len)
{
    if (!node || !state_data || state_len == 0) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    node->last_applied = node->commit_index;
    csilk_mutex_unlock(&node->mutex);

    return 0;
}

int
csilk_raft_snapshot_restore(csilk_raft_node_t* node,
                            const uint8_t*     snapshot_data,
                            size_t             snapshot_len)
{
    if (!node || !snapshot_data || snapshot_len == 0) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    node->commit_index = node->last_applied;
    csilk_mutex_unlock(&node->mutex);

    return 0;
}
