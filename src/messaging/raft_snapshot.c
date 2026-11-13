/**
 * @file raft_snapshot.c
 * @brief Raft snapshot persistence, WAL prefix compaction, and restoration.
 * @copyright MIT License
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "raft_internal.h"

static uint32_t
raft_snap_hdr_crc32(const csilk_raft_snap_disk_hdr_t* hdr)
{
    return _csilk_wal_crc32(0, hdr, offsetof(csilk_raft_snap_disk_hdr_t, hdr_crc32));
}

int
_raft_snapshot_load_disk_locked(csilk_raft_node_t* node)
{
    if (!node || !node->snap_path) {
        return 0;
    }

    int fd = open(node->snap_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    csilk_raft_snap_disk_hdr_t hdr;
    int                        rrc = _csilk_wal_read_all(fd, &hdr, sizeof(hdr));
    if (rrc != 0) {
        close(fd);
        return -1;
    }

    if (hdr.magic != CSILK_RAFT_SNAP_MAGIC || hdr.version != CSILK_RAFT_FORMAT_VERSION ||
        hdr.state_len == 0 || hdr.state_len > CSILK_RAFT_MAX_FRAME_DATA ||
        hdr.hdr_crc32 != raft_snap_hdr_crc32(&hdr)) {
        close(fd);
        return -1;
    }

    uint8_t* state = malloc((size_t)hdr.state_len);
    if (!state) {
        close(fd);
        return -1;
    }

    if (_csilk_wal_read_all(fd, state, (size_t)hdr.state_len) != 0) {
        free(state);
        close(fd);
        return -1;
    }

    uint32_t state_crc = 0;
    if (_csilk_wal_read_all(fd, &state_crc, sizeof(state_crc)) != 0) {
        free(state);
        close(fd);
        return -1;
    }
    close(fd);

    if (state_crc != _csilk_wal_crc32(0, state, (size_t)hdr.state_len)) {
        free(state);
        return -1;
    }

    free(node->snapshot_data);
    node->snapshot_data = state;
    node->snapshot_len = (size_t)hdr.state_len;
    node->snapshot_last_index = hdr.last_included_index;
    node->snapshot_last_term = hdr.last_included_term;

    if (hdr.last_included_index > node->log_base_index) {
        node->log_base_index = hdr.last_included_index;
        node->log_base_term = hdr.last_included_term;
    }
    if (node->last_log_index < hdr.last_included_index) {
        node->last_log_index = hdr.last_included_index;
    }
    if (node->commit_index < hdr.last_included_index) {
        node->commit_index = hdr.last_included_index;
    }

    return 0;
}

int
_raft_snapshot_install_locked(csilk_raft_node_t* node,
                              uint64_t           last_included_index,
                              uint64_t           last_included_term,
                              const uint8_t*     data,
                              size_t             len)
{
    if (!node || !data || len == 0 || len > CSILK_RAFT_MAX_FRAME_DATA) {
        return -1;
    }

    uint8_t* mem_copy = malloc(len);
    if (!mem_copy) {
        return -1;
    }
    memcpy(mem_copy, data, len);

    if (node->snap_path) {
        size_t   total = sizeof(csilk_raft_snap_disk_hdr_t) + len + sizeof(uint32_t);
        uint8_t* disk_buf = malloc(total);
        if (!disk_buf) {
            free(mem_copy);
            return -1;
        }

        csilk_raft_snap_disk_hdr_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = CSILK_RAFT_SNAP_MAGIC;
        hdr.version = CSILK_RAFT_FORMAT_VERSION;
        hdr.last_included_index = last_included_index;
        hdr.last_included_term = last_included_term;
        hdr.state_len = (uint64_t)len;
        hdr.hdr_crc32 = raft_snap_hdr_crc32(&hdr);

        uint32_t state_crc = _csilk_wal_crc32(0, data, len);

        memcpy(disk_buf, &hdr, sizeof(hdr));
        memcpy(disk_buf + sizeof(hdr), data, len);
        memcpy(disk_buf + sizeof(hdr) + len, &state_crc, sizeof(state_crc));

        int wrc = _csilk_wal_write_atomic(node->snap_path, disk_buf, total);
        free(disk_buf);
        if (wrc != 0) {
            free(mem_copy);
            return -1;
        }
    }

    free(node->snapshot_data);
    node->snapshot_data = mem_copy;
    node->snapshot_len = len;
    node->snapshot_last_index = last_included_index;
    node->snapshot_last_term = last_included_term;

    if (last_included_index > node->log_base_index) {
        if (_raft_wal_compact_prefix_locked(node, last_included_index, last_included_term) != 0) {
            return -1;
        }
    }

    return 0;
}

int
csilk_raft_snapshot_create(csilk_raft_node_t* node, const uint8_t* state_data, size_t state_len)
{
    if (!node || !state_data || state_len == 0) {
        return -1;
    }

    csilk_mutex_lock(&node->mutex);
    uint64_t snap_idx = node->commit_index;
    uint64_t snap_term = _raft_log_term_at_locked(node, snap_idx);

    if (_raft_snapshot_install_locked(node, snap_idx, snap_term, state_data, state_len) != 0) {
        csilk_mutex_unlock(&node->mutex);
        return -1;
    }
    node->last_applied = snap_idx;
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

    uint8_t* copy = malloc(snapshot_len);
    if (!copy) {
        return -1;
    }
    memcpy(copy, snapshot_data, snapshot_len);

    csilk_mutex_lock(&node->mutex);
    free(node->snapshot_data);
    node->snapshot_data = copy;
    node->snapshot_len = snapshot_len;
    node->commit_index = node->last_applied;
    uint64_t                       snap_idx = node->snapshot_last_index;
    uint64_t                       snap_term = node->snapshot_last_term;
    csilk_raft_snapshot_apply_cb_t cb = node->snapshot_apply_cb;
    void*                          ud = node->apply_user_data;
    csilk_mutex_unlock(&node->mutex);

    if (cb) {
        cb(snap_idx, snap_term, snapshot_data, snapshot_len, ud);
    }

    return 0;
}

int
csilk_raft_snapshot_load(const csilk_raft_node_t* node,
                         uint64_t*                out_last_index,
                         uint64_t*                out_last_term,
                         uint8_t**                out_data,
                         size_t*                  out_len)
{
    if (!node) {
        return -1;
    }

    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    if (!node->snapshot_data || node->snapshot_len == 0) {
        csilk_mutex_unlock(&mutable_node->mutex);
        return -1;
    }

    if (out_data) {
        uint8_t* copy = malloc(node->snapshot_len);
        if (!copy) {
            csilk_mutex_unlock(&mutable_node->mutex);
            return -1;
        }
        memcpy(copy, node->snapshot_data, node->snapshot_len);
        *out_data = copy;
    }
    if (out_last_index) {
        *out_last_index = node->snapshot_last_index;
    }
    if (out_last_term) {
        *out_last_term = node->snapshot_last_term;
    }
    if (out_len) {
        *out_len = node->snapshot_len;
    }
    csilk_mutex_unlock(&mutable_node->mutex);
    return 0;
}
