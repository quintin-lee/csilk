/**
 * @file raft_snapshot.c
 * @brief Raft snapshot create/restore — state machine checkpointing.
 *
 * Implements snapshot lifecycle hooks.  Creating a snapshot advances
 * last_applied to the current commit index; restoring from a snapshot sets
 * commit_index back to last_applied.  (State bytes are accepted for API
 * symmetry but not yet persisted by this stub.)
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

/**
 * @brief Create a snapshot of the current committed state.
 *
 * Marks the state machine checkpoint by setting last_applied = commit_index
 * under node->mutex.  The provided @p state_data is validated for non-empty but
 * not stored by this implementation.
 *
 * @param[in] node        Node instance (must not be NULL).
 * @param[in] state_data  Snapshot state bytes (must not be NULL, non-empty).
 * @param[in] state_len   Length of @p state_data in bytes.
 * @return 0 on success, -1 on NULL arguments or empty state.
 */
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

/**
 * @brief Restore node state from a snapshot.
 *
 * Marks replay by setting commit_index = last_applied under node->mutex.  The
 * provided @p snapshot_data is validated for non-empty but not loaded by this
 * implementation.
 *
 * @param[in] node           Node instance (must not be NULL).
 * @param[in] snapshot_data  Snapshot state bytes (must not be NULL, non-empty).
 * @param[in] snapshot_len   Length of @p snapshot_data in bytes.
 * @return 0 on success, -1 on NULL arguments or empty snapshot.
 */
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
