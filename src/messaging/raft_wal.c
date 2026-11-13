/**
 * @file raft_wal.c
 * @brief Distributed Raft consensus WAL log replication and persistence engine.
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
raft_meta_crc32(const csilk_raft_meta_disk_t* disk)
{
    return _csilk_wal_crc32(0, disk, offsetof(csilk_raft_meta_disk_t, crc32));
}

static uint32_t
raft_wal_hdr_crc32(const csilk_raft_wal_rec_hdr_t* hdr)
{
    return _csilk_wal_crc32(0, hdr, offsetof(csilk_raft_wal_rec_hdr_t, hdr_crc32));
}

static uint32_t
raft_xorshift32(uint32_t* state)
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
raft_pick_election_timeout(csilk_raft_node_t* node)
{
    uint64_t min_ms = node->config.election_timeout_min_ms;
    uint64_t max_ms = node->config.election_timeout_max_ms;
    if (max_ms <= min_ms) {
        return min_ms;
    }
    uint64_t span = max_ms - min_ms + 1ULL;
    return min_ms + (uint64_t)(raft_xorshift32(&node->rng_state) % (uint32_t)span);
}

static int
raft_write_wal_record_fd(int fd, uint64_t index, uint64_t term, const uint8_t* data, size_t len)
{
    if (len > CSILK_RAFT_MAX_FRAME_DATA) {
        return -1;
    }

    csilk_raft_wal_rec_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CSILK_RAFT_LOG_MAGIC;
    hdr.index = index;
    hdr.term = term;
    hdr.data_len = (uint32_t)len;
    hdr.hdr_crc32 = raft_wal_hdr_crc32(&hdr);

    uint32_t payload_crc = _csilk_wal_crc32(0, data, len);

    if (_csilk_wal_write_all(fd, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (len > 0 && _csilk_wal_write_all(fd, data, len) != 0) {
        return -1;
    }
    if (_csilk_wal_write_all(fd, &payload_crc, sizeof(payload_crc)) != 0) {
        return -1;
    }
    return 0;
}

static int
raft_entries_push_mem(csilk_raft_node_t* node,
                      uint64_t           index,
                      uint64_t           term,
                      const uint8_t*     data,
                      size_t             len,
                      uint64_t           wal_offset)
{
    if (node->entry_count >= node->entry_capacity) {
        size_t                  new_cap = node->entry_capacity == 0 ? 16 : node->entry_capacity * 2;
        csilk_raft_log_entry_t* grown =
            realloc(node->entries, new_cap * sizeof(csilk_raft_log_entry_t));
        if (!grown) {
            return -1;
        }
        node->entries = grown;
        node->entry_capacity = new_cap;
    }

    uint8_t* copy = NULL;
    if (len > 0) {
        copy = malloc(len);
        if (!copy) {
            return -1;
        }
        memcpy(copy, data, len);
    }

    csilk_raft_log_entry_t* ent = &node->entries[node->entry_count++];
    ent->index = index;
    ent->term = term;
    ent->data = copy;
    ent->len = len;
    ent->wal_offset = wal_offset;
    return 0;
}

static int
raft_load_meta_disk(csilk_raft_node_t* node)
{
    if (!node->meta_path) {
        return 0;
    }

    int fd = open(node->meta_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    csilk_raft_meta_disk_t disk;
    int                    rrc = _csilk_wal_read_all(fd, &disk, sizeof(disk));
    close(fd);
    if (rrc != 0) {
        return -1;
    }

    if (disk.magic != CSILK_RAFT_META_MAGIC || disk.version != CSILK_RAFT_FORMAT_VERSION) {
        return -1;
    }
    if (disk.crc32 != raft_meta_crc32(&disk)) {
        return -1;
    }

    node->current_term = disk.current_term;
    if (disk.has_voted_for) {
        memcpy(node->voted_for, disk.voted_for, sizeof(node->voted_for));
        node->voted_for[sizeof(node->voted_for) - 1] = '\0';
    } else {
        node->voted_for[0] = '\0';
    }
    return 0;
}

int
_raft_wal_save_meta_locked(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }
    if (!node->meta_path) {
        return 0;
    }

    csilk_raft_meta_disk_t disk;
    memset(&disk, 0, sizeof(disk));
    disk.magic = CSILK_RAFT_META_MAGIC;
    disk.version = CSILK_RAFT_FORMAT_VERSION;
    disk.current_term = node->current_term;
    if (node->voted_for[0] != '\0') {
        disk.has_voted_for = 1U;
        snprintf(disk.voted_for, sizeof(disk.voted_for), "%s", node->voted_for);
    }
    disk.crc32 = raft_meta_crc32(&disk);

    return _csilk_wal_write_atomic(node->meta_path, &disk, sizeof(disk));
}

int
_raft_wal_open_and_recover(csilk_raft_node_t* node)
{
    if (!node || !node->config.wal_dir || node->config.wal_dir[0] == '\0') {
        return 0;
    }

    if (_csilk_wal_mkdir_p(node->config.wal_dir) != 0) {
        return -1;
    }

    size_t dir_len = strlen(node->config.wal_dir);
    node->wal_dir = strdup(node->config.wal_dir);
    node->meta_path = malloc(dir_len + 32U);
    node->wal_path = malloc(dir_len + 32U);
    node->snap_path = malloc(dir_len + 32U);
    if (!node->wal_dir || !node->meta_path || !node->wal_path || !node->snap_path) {
        return -1;
    }

    snprintf(node->meta_path, dir_len + 32U, "%s/meta.bin", node->wal_dir);
    snprintf(node->wal_path, dir_len + 32U, "%s/raft.wal", node->wal_dir);
    snprintf(node->snap_path, dir_len + 32U, "%s/snapshot.bin", node->wal_dir);

    if (raft_load_meta_disk(node) != 0) {
        return -1;
    }

    /* Load snapshot before replaying WAL so compacted prefix is known. */
    if (_raft_snapshot_load_disk_locked(node) != 0) {
        return -1;
    }

    int fd = open(node->wal_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return -1;
    }
    node->wal_fd = fd;

    uint64_t valid_offset = 0;
    int      need_truncate = 0;

    for (;;) {
        csilk_raft_wal_rec_hdr_t hdr;
        int                      rrc = _csilk_wal_read_all(fd, &hdr, sizeof(hdr));
        if (rrc == 1) {
            break; /* Clean EOF at record boundary */
        }
        if (rrc != 0) {
            need_truncate = 1;
            break;
        }

        if (hdr.magic != CSILK_RAFT_LOG_MAGIC || hdr.data_len > CSILK_RAFT_MAX_FRAME_DATA ||
            hdr.hdr_crc32 != raft_wal_hdr_crc32(&hdr)) {
            need_truncate = 1;
            break;
        }

        uint8_t* payload = NULL;
        if (hdr.data_len > 0) {
            payload = malloc(hdr.data_len);
            if (!payload) {
                return -1;
            }
            if (_csilk_wal_read_all(fd, payload, hdr.data_len) != 0) {
                free(payload);
                need_truncate = 1;
                break;
            }
        }

        uint32_t payload_crc = 0;
        if (_csilk_wal_read_all(fd, &payload_crc, sizeof(payload_crc)) != 0) {
            free(payload);
            need_truncate = 1;
            break;
        }

        if (payload_crc != _csilk_wal_crc32(0, payload, hdr.data_len)) {
            free(payload);
            need_truncate = 1;
            break;
        }

        uint64_t rec_offset = valid_offset;
        valid_offset += sizeof(hdr) + (uint64_t)hdr.data_len + sizeof(payload_crc);

        if (hdr.index > node->log_base_index) {
            if (hdr.index != node->last_log_index + 1ULL) {
                free(payload);
                valid_offset = rec_offset;
                need_truncate = 1;
                break;
            }
            if (raft_entries_push_mem(
                    node, hdr.index, hdr.term, payload, hdr.data_len, rec_offset) != 0) {
                free(payload);
                return -1;
            }
            node->last_log_index = hdr.index;
        }

        free(payload);
    }

    if (need_truncate) {
        if (ftruncate(fd, (off_t)valid_offset) != 0) {
            return -1;
        }
        (void)_csilk_wal_fsync_fd(fd);
    }

    if (lseek(fd, (off_t)valid_offset, SEEK_SET) < 0) {
        return -1;
    }
    node->wal_file_size = valid_offset;
    return 0;
}

void
_raft_wal_close(csilk_raft_node_t* node)
{
    if (!node) {
        return;
    }
    if (node->wal_fd >= 0) {
        close(node->wal_fd);
        node->wal_fd = -1;
    }
}

int
_raft_wal_flush_locked(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }
    if (node->wal_fd < 0) {
        node->pending_sync_entries = 0;
        return 0;
    }
    if (node->pending_sync_entries == 0) {
        return 0;
    }
    if (_csilk_wal_fsync_fd(node->wal_fd) != 0) {
        return -1;
    }
    node->sync_count++;
    node->pending_sync_entries = 0;
    node->last_sync_ms = _csilk_raft_now_ms();
    return 0;
}

int
_raft_wal_append_locked(
    csilk_raft_node_t* node, uint64_t term, const uint8_t* data, size_t len, uint64_t* out_index)
{
    if (!node || (len > 0 && !data) || len > CSILK_RAFT_MAX_FRAME_DATA) {
        return -1;
    }

    uint64_t next_index = node->last_log_index + 1ULL;
    uint64_t wal_offset = node->wal_file_size;

    if (node->wal_fd >= 0) {
        if (raft_write_wal_record_fd(node->wal_fd, next_index, term, data, len) != 0) {
            (void)ftruncate(node->wal_fd, (off_t)wal_offset);
            (void)lseek(node->wal_fd, (off_t)wal_offset, SEEK_SET);
            return -1;
        }
        node->wal_file_size =
            wal_offset + sizeof(csilk_raft_wal_rec_hdr_t) + (uint64_t)len + sizeof(uint32_t);
        node->pending_sync_entries++;
    }

    if (raft_entries_push_mem(node, next_index, term, data, len, wal_offset) != 0) {
        if (node->wal_fd >= 0) {
            (void)ftruncate(node->wal_fd, (off_t)wal_offset);
            (void)lseek(node->wal_fd, (off_t)wal_offset, SEEK_SET);
            node->wal_file_size = wal_offset;
            if (node->pending_sync_entries > 0) {
                node->pending_sync_entries--;
            }
        }
        return -1;
    }

    node->last_log_index = next_index;

    if (node->wal_fd >= 0) {
        int need_sync = 0;
        switch (node->config.sync_policy) {
        case CSILK_RAFT_SYNC_ALWAYS:
            need_sync = 1;
            break;
        case CSILK_RAFT_SYNC_BATCH:
            if (node->pending_sync_entries >= node->config.sync_batch_size) {
                need_sync = 1;
            }
            break;
        case CSILK_RAFT_SYNC_INTERVAL_MS: {
            uint64_t now = _csilk_raft_now_ms();
            if (now >= node->last_sync_ms &&
                (now - node->last_sync_ms) >= (uint64_t)node->config.sync_interval_ms) {
                need_sync = 1;
            }
            break;
        }
        default:
            need_sync = 1;
            break;
        }

        if (need_sync && _raft_wal_flush_locked(node) != 0) {
            return -1;
        }
    }

    if (out_index) {
        *out_index = next_index;
    }
    return 0;
}

int
_raft_wal_truncate_from_locked(csilk_raft_node_t* node, uint64_t from_index)
{
    if (!node || from_index <= node->log_base_index) {
        return -1;
    }
    if (from_index > node->last_log_index) {
        return 0;
    }

    size_t pos = (size_t)(from_index - node->log_base_index - 1ULL);
    if (pos >= node->entry_count) {
        return 0;
    }

    uint64_t new_wal_size = node->entries[pos].wal_offset;
    for (size_t i = pos; i < node->entry_count; i++) {
        free(node->entries[i].data);
        node->entries[i].data = NULL;
    }
    node->entry_count = pos;
    node->last_log_index = from_index - 1ULL;

    if (node->commit_index > node->last_log_index) {
        node->commit_index = node->last_log_index;
    }

    if (node->wal_fd >= 0) {
        if (ftruncate(node->wal_fd, (off_t)new_wal_size) != 0) {
            return -1;
        }
        if (lseek(node->wal_fd, (off_t)new_wal_size, SEEK_SET) < 0) {
            return -1;
        }
        if (_csilk_wal_fsync_fd(node->wal_fd) != 0) {
            return -1;
        }
        node->wal_file_size = new_wal_size;
        node->pending_sync_entries = 0;
        node->sync_count++;
    }
    return 0;
}

int
_raft_wal_compact_prefix_locked(csilk_raft_node_t* node,
                                uint64_t           last_included_index,
                                uint64_t           last_included_term)
{
    if (!node) {
        return -1;
    }
    if (last_included_index <= node->log_base_index) {
        return 0;
    }

    size_t drop_count = 0;
    while (drop_count < node->entry_count &&
           node->entries[drop_count].index <= last_included_index) {
        free(node->entries[drop_count].data);
        node->entries[drop_count].data = NULL;
        drop_count++;
    }

    size_t remain = node->entry_count - drop_count;
    if (remain > 0 && drop_count > 0) {
        memmove(
            &node->entries[0], &node->entries[drop_count], remain * sizeof(csilk_raft_log_entry_t));
    }
    node->entry_count = remain;
    node->log_base_index = last_included_index;
    node->log_base_term = last_included_term;

    if (node->last_log_index < last_included_index) {
        node->last_log_index = last_included_index;
    }
    if (node->commit_index < last_included_index) {
        node->commit_index = last_included_index;
    }

    if (node->wal_fd >= 0 && node->wal_path) {
        size_t tmp_cap = strlen(node->wal_path) + 16U;
        char*  tmp_path = malloc(tmp_cap);
        if (!tmp_path) {
            return -1;
        }
        snprintf(tmp_path, tmp_cap, "%s.tmp", node->wal_path);

        int tmp_fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (tmp_fd < 0) {
            free(tmp_path);
            return -1;
        }

        uint64_t offset = 0;
        for (size_t i = 0; i < node->entry_count; i++) {
            csilk_raft_log_entry_t* ent = &node->entries[i];
            if (raft_write_wal_record_fd(tmp_fd, ent->index, ent->term, ent->data, ent->len) != 0) {
                close(tmp_fd);
                unlink(tmp_path);
                free(tmp_path);
                return -1;
            }
            ent->wal_offset = offset;
            offset += sizeof(csilk_raft_wal_rec_hdr_t) + (uint64_t)ent->len + sizeof(uint32_t);
        }

        if (_csilk_wal_fsync_fd(tmp_fd) != 0) {
            close(tmp_fd);
            unlink(tmp_path);
            free(tmp_path);
            return -1;
        }
        close(tmp_fd);

        close(node->wal_fd);
        node->wal_fd = -1;

        if (rename(tmp_path, node->wal_path) != 0) {
            unlink(tmp_path);
            free(tmp_path);
            node->wal_fd = open(node->wal_path, O_RDWR | O_CREAT, 0644);
            return -1;
        }
        (void)_csilk_wal_fsync_dir(node->wal_path);
        free(tmp_path);

        node->wal_fd = open(node->wal_path, O_RDWR | O_CREAT, 0644);
        if (node->wal_fd < 0) {
            return -1;
        }
        if (lseek(node->wal_fd, (off_t)offset, SEEK_SET) < 0) {
            return -1;
        }
        node->wal_file_size = offset;
        node->pending_sync_entries = 0;
        node->sync_count++;
    }

    return 0;
}

const csilk_raft_log_entry_t*
_raft_log_get_locked(const csilk_raft_node_t* node, uint64_t index)
{
    if (!node || index <= node->log_base_index || index > node->last_log_index) {
        return NULL;
    }
    size_t pos = (size_t)(index - node->log_base_index - 1ULL);
    if (pos >= node->entry_count) {
        return NULL;
    }
    return &node->entries[pos];
}

uint64_t
_raft_log_term_at_locked(const csilk_raft_node_t* node, uint64_t index)
{
    if (!node || index == 0) {
        return 0;
    }
    if (index == node->log_base_index) {
        return node->log_base_term;
    }
    const csilk_raft_log_entry_t* ent = _raft_log_get_locked(node, index);
    return ent ? ent->term : 0;
}

/* ========================================================================= */
/* Public WAL & Node Lifecycle API                                           */
/* ========================================================================= */

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
    node->config.wal_dir = NULL; /* Replaced by heap-owned node->wal_dir */
    if (node->config.cluster_size == 0) {
        node->config.cluster_size = 3;
    }
    if (node->config.sync_interval_ms == 0) {
        node->config.sync_interval_ms = 100;
    }
    if (node->config.sync_batch_size == 0) {
        node->config.sync_batch_size = 16;
    }
    if (node->config.heartbeat_interval_ms == 0) {
        node->config.heartbeat_interval_ms = 50;
    }
    if (node->config.election_timeout_min_ms == 0) {
        node->config.election_timeout_min_ms = 150;
    }
    if (node->config.election_timeout_max_ms == 0) {
        node->config.election_timeout_max_ms = 300;
    }
    if (node->config.election_timeout_max_ms < node->config.election_timeout_min_ms) {
        node->config.election_timeout_max_ms = node->config.election_timeout_min_ms + 150ULL;
    }

    node->role = CSILK_RAFT_ROLE_FOLLOWER;
    node->current_term = 1;
    node->wal_fd = -1;
    node->listen_fd = -1;
    csilk_mutex_init(&node->mutex);
    csilk_cond_init(&node->commit_cond);

    uint64_t now = _csilk_raft_now_ms();
    node->last_heartbeat_time = now;
    node->last_sync_ms = now;
    node->rng_state =
        (uint32_t)(config->node_id * 2654435761U) ^ (uint32_t)now ^ (uint32_t)(uintptr_t)node;
    if (node->rng_state == 0) {
        node->rng_state = 0x12345678U;
    }
    node->election_timeout_ms = raft_pick_election_timeout(node);

    if (config->wal_dir && config->wal_dir[0] != '\0') {
        /* Temporarily point config.wal_dir for _raft_wal_open_and_recover */
        node->config.wal_dir = config->wal_dir;
        int rc = _raft_wal_open_and_recover(node);
        node->config.wal_dir = node->wal_dir;
        if (rc != 0) {
            csilk_raft_node_free(node);
            return NULL;
        }
    }

    return node;
}

void
csilk_raft_node_free(csilk_raft_node_t* node)
{
    if (!node) {
        return;
    }

    (void)csilk_raft_node_stop(node);

    csilk_mutex_lock(&node->mutex);
    if (node->wal_fd >= 0 && node->pending_sync_entries > 0) {
        (void)_raft_wal_flush_locked(node);
    }
    _raft_wal_close(node);

    for (size_t i = 0; i < node->entry_count; i++) {
        free(node->entries[i].data);
    }
    free(node->entries);
    free(node->snapshot_data);
    free(node->snap_rx_buf);
    free(node->wal_dir);
    free(node->meta_path);
    free(node->wal_path);
    free(node->snap_path);
    csilk_mutex_unlock(&node->mutex);

    csilk_cond_destroy(&node->commit_cond);
    csilk_mutex_destroy(&node->mutex);
    free(node);
}

uint64_t
csilk_raft_append_log(csilk_raft_node_t* node, const uint8_t* data, size_t len)
{
    if (!node || !data || len == 0) {
        return 0;
    }

    csilk_mutex_lock(&node->mutex);
    if (node->role != CSILK_RAFT_ROLE_LEADER) {
        csilk_mutex_unlock(&node->mutex);
        return 0;
    }

    uint64_t idx = 0;
    if (_raft_wal_append_locked(node, node->current_term, data, len, &idx) != 0) {
        csilk_mutex_unlock(&node->mutex);
        return 0;
    }
    csilk_mutex_unlock(&node->mutex);
    return idx;
}

uint64_t
csilk_raft_quorum_ack(csilk_raft_node_t* node, size_t ack_nodes)
{
    if (!node) {
        return 0;
    }

    csilk_mutex_lock(&node->mutex);
    size_t majority = (node->config.cluster_size / 2U) + 1U;
    if (ack_nodes >= majority && node->commit_index < node->last_log_index) {
        node->commit_index = node->last_log_index;
        _raft_apply_committed_locked(node);
        csilk_cond_broadcast(&node->commit_cond);
    }
    uint64_t committed = node->commit_index;
    csilk_mutex_unlock(&node->mutex);
    return committed;
}

int
csilk_raft_wal_flush(csilk_raft_node_t* node)
{
    if (!node) {
        return -1;
    }
    csilk_mutex_lock(&node->mutex);
    int rc = _raft_wal_flush_locked(node);
    csilk_mutex_unlock(&node->mutex);
    return rc;
}

uint64_t
csilk_raft_wal_sync_count(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint64_t cnt = node->sync_count;
    csilk_mutex_unlock(&mutable_node->mutex);
    return cnt;
}

csilk_raft_role_t
csilk_raft_get_role(const csilk_raft_node_t* node)
{
    if (!node) {
        return CSILK_RAFT_ROLE_FOLLOWER;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    csilk_raft_role_t role = node->role;
    csilk_mutex_unlock(&mutable_node->mutex);
    return role;
}

uint64_t
csilk_raft_get_term(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint64_t term = node->current_term;
    csilk_mutex_unlock(&mutable_node->mutex);
    return term;
}

uint64_t
csilk_raft_get_commit_index(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint64_t ci = node->commit_index;
    csilk_mutex_unlock(&mutable_node->mutex);
    return ci;
}

uint64_t
csilk_raft_get_last_log_index(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint64_t idx = node->last_log_index;
    csilk_mutex_unlock(&mutable_node->mutex);
    return idx;
}

uint64_t
csilk_raft_get_last_log_term(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint64_t term = _raft_log_term_at_locked(node, node->last_log_index);
    csilk_mutex_unlock(&mutable_node->mutex);
    return term;
}

uint64_t
csilk_raft_get_last_applied(const csilk_raft_node_t* node)
{
    if (!node) {
        return 0;
    }
    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    uint64_t applied = node->last_applied;
    csilk_mutex_unlock(&mutable_node->mutex);
    return applied;
}

int
csilk_raft_get_log_entry(const csilk_raft_node_t* node,
                         uint64_t                 index,
                         uint64_t*                out_term,
                         uint8_t**                out_data,
                         size_t*                  out_len)
{
    if (!node || index == 0) {
        return -1;
    }

    csilk_raft_node_t* mutable_node = (csilk_raft_node_t*)node;
    csilk_mutex_lock(&mutable_node->mutex);
    const csilk_raft_log_entry_t* ent = _raft_log_get_locked(node, index);
    if (!ent) {
        csilk_mutex_unlock(&mutable_node->mutex);
        return -1;
    }

    if (out_data) {
        if (ent->len > 0) {
            uint8_t* copy = malloc(ent->len);
            if (!copy) {
                csilk_mutex_unlock(&mutable_node->mutex);
                return -1;
            }
            memcpy(copy, ent->data, ent->len);
            *out_data = copy;
        } else {
            *out_data = NULL;
        }
    }
    if (out_term) {
        *out_term = ent->term;
    }
    if (out_len) {
        *out_len = ent->len;
    }
    csilk_mutex_unlock(&mutable_node->mutex);
    return 0;
}
