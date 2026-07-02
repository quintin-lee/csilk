/**
 * @file mq_wal.c
 * @brief MQ Write-Ahead Log persistence — durable message delivery.
 *
 * Implements the WAL-backed persistence layer for the Message Queue:
 *   - _mq_append_wal(): append a message frame to the WAL file with
 *     XOR checksum for integrity.
 *   - _mq_recovery(): replay the WAL on startup to restore undelivered
 *     messages into the in-memory queue.
 *   - csilk_mq_set_persistence(): enable WAL persistence for an MQ instance.
 *
 * WAL frame format: [topic_len:4][topic:N][payload_len:4][payload:M][xor:4]
 * @copyright MIT License
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "mq_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "csilk/mq.h"

#include "mq_internal.h"

/** @brief Append a message frame to the WAL file on disk.
 *
 * Serializes the message into a self-delimiting frame and writes to the
 * WAL file descriptor. Each frame includes an XOR checksum over the
 * topic + payload bytes for integrity verification during recovery.
 *
 * ## Frame wire format (16 bytes overhead per message)
 * ```
 * +0: topic_len   (uint32_t, 4 bytes, little-endian)
 * +4: topic       (topic_len bytes, NOT null-terminated on disk)
 * +N: payload_len (uint32_t, 4 bytes)
 * +M: payload     (payload_len bytes)
 * +K: checksum    (uint32_t, 4 bytes, XOR of all topic + payload bytes)
 * ```
 *
 * ## Integrity
 * The XOR checksum is a lightweight, non-cryptographic integrity check.
 * It detects single-bit flips and many classes of corruption, but does
 * NOT protect against malicious tampering.
 *
 * ## Performance notes
 *   - Uses a single csilk_io_fs_write with a 5-element scatter/gather buffer to
 *     avoid extra memory copies.
 *   - Calls csilk_io_fs_fsync() after every write for durability. For
 *     high-throughput scenarios, consider batching.
 *
 * ## Call chain
 *   1. csilk_mq_publish(mq, topic, payload, len)
 *       └─ _mq_append_wal(mq, topic, payload, len)   ← here
 *           └─ csilk_io_fs_write()
 *           └─ csilk_io_fs_fsync()
 *       └─ _mq_enqueue(mq, topic, payload, len)
 *
 * @param mq      MQ instance (must have wal_fd >= 0).
 * @param topic   Topic string (not nullptr).
 * @param payload Opaque payload data (may be nullptr if len == 0).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 on write or fsync failure.
 * @threadsafe Serialized via wal_mutex. */
CSILK_INTERNAL int
_mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
    if (!mq || mq->wal_fd < 0 || !topic) {
        return 0;
    }

    csilk_mutex_lock(&mq->wal_mutex);

    uint32_t topic_len = (uint32_t)strlen(topic);
    uint32_t payload_len = (uint32_t)len;
    uint32_t checksum = 0;

    for (uint32_t i = 0; i < topic_len; i++) {
        checksum ^= (uint8_t)topic[i];
    }
    const uint8_t* p = (const uint8_t*)payload;
    if (p) {
        for (uint32_t i = 0; i < payload_len; i++) {
            checksum ^= p[i];
        }
    }

    csilk_io_buf_t bufs[5];
    bufs[0] = csilk_io_buf_init((char*)&topic_len, 4);
    bufs[1] = csilk_io_buf_init((char*)topic, topic_len);
    bufs[2] = csilk_io_buf_init((char*)&payload_len, 4);
    bufs[3] = csilk_io_buf_init((char*)payload, payload_len);
    bufs[4] = csilk_io_buf_init((char*)&checksum, 4);

    CSILK_LOG_T("MQ: Appending message to WAL. Topic: '%s', Length: %zu, Checksum: 0x%08x",
                topic,
                len,
                checksum);

    csilk_io_fs_t write_req;
    int result = csilk_io_fs_write(mq->loop, &write_req, mq->wal_fd, bufs, 5, -1, nullptr);
    csilk_io_fs_req_cleanup(&write_req);

    if (result >= 0) {
        csilk_io_fs_t sync_req;
        csilk_io_fs_fsync(mq->loop, &sync_req, mq->wal_fd, nullptr);
        csilk_io_fs_req_cleanup(&sync_req);
        CSILK_LOG_D("MQ: Successfully appended and synced message to WAL for topic '%s'", topic);
    } else {
        CSILK_LOG_E("MQ: Failed to write message to WAL (error: %d) for topic '%s'", result, topic);
    }

    csilk_mutex_unlock(&mq->wal_mutex);
    return (result >= 0) ? 0 : -1;
}

/** @brief Recover undelivered messages from the WAL on startup.
 *
 * Reads the WAL file sequentially from offset 0 using positional reads
 * (csilk_io_fs_read with offset parameter). Each frame is validated via its XOR
 * checksum; valid frames are re-enqueued into the in-memory queue.
 *
 * ## Recovery algorithm (per frame)
 * ```
 * 1. Read 4 bytes  → topic_len
 * 2. Read N bytes  → topic       (N = topic_len)
 * 3. Read 4 bytes  → payload_len
 * 4. Read M bytes  → payload     (M = payload_len)
 * 5. Read 4 bytes  → stored_checksum
 * 6. Compute XOR checksum over topic + payload bytes
 * 7. If computed == stored → _mq_enqueue()
 * 8. Else → free partial data, break (corruption boundary)
 * ```
 *
 * ## Why stop at corruption?
 * Without a frame-length prefix, a corrupt topic_len or payload_len makes
 * the next frame boundary unknowable. We stop rather than misinterpret
 * garbage as valid messages.
 *
 * ## Invariant
 * The WAL is NOT truncated after recovery. New appends write after existing
 * data. The in-memory queue reflects exactly the subset of frames that
 * passed checksum validation.
 *
 * @param mq MQ instance (must have wal_fd >= 0).
 * @return 0 always (errors are non-fatal — we stop and return).
 * @note Called from csilk_mq_set_persistence() under wal_mutex. */
CSILK_INTERNAL int
_mq_recovery(csilk_mq_t* mq)
{
    if (!mq || mq->wal_fd < 0) {
        return 0;
    }

    CSILK_LOG_I("MQ: Starting WAL recovery from path '%s'",
                mq->wal_path ? mq->wal_path : "unknown");

    uint64_t offset = 0; /* byte position in the WAL file */
    size_t   recovered_count = 0;

    /* Read frames until EOF or corruption */
    while (1) {
        uint32_t      topic_len = 0;
        uint32_t      payload_len = 0;
        uint32_t      checksum = 0;
        csilk_io_fs_t read_req;
        int           nread;

        /* 1. Read topic_len (4 bytes) */
        csilk_io_buf_t buf = csilk_io_buf_init((char*)&topic_len, 4);
        nread = csilk_io_fs_read(mq->loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
        csilk_io_fs_req_cleanup(&read_req);
        if (nread < 4) {
            if (nread > 0) {
                CSILK_LOG_W("MQ: WAL recovery stopped at offset %zu: truncated topic_len",
                            (size_t)offset);
            }
            break; /* EOF or truncated frame — stop */
        }
        offset += 4;

        /* 2. Read topic bytes */
        char* topic = malloc(topic_len + 1);
        if (!topic) {
            CSILK_LOG_E("MQ: WAL recovery memory allocation failed for topic at offset %zu",
                        (size_t)offset);
            break;
        }
        buf = csilk_io_buf_init(topic, topic_len);
        nread = csilk_io_fs_read(mq->loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
        csilk_io_fs_req_cleanup(&read_req);
        if (nread < (int)topic_len) {
            CSILK_LOG_W("MQ: WAL recovery stopped at offset %zu: truncated topic bytes",
                        (size_t)offset);
            free(topic);
            break;
        }
        topic[topic_len] = '\0'; /* null-terminate for string safety */
        offset += topic_len;

        /* 3. Read payload_len (4 bytes) */
        buf = csilk_io_buf_init((char*)&payload_len, 4);
        nread = csilk_io_fs_read(mq->loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
        csilk_io_fs_req_cleanup(&read_req);
        if (nread < 4) {
            CSILK_LOG_W("MQ: WAL recovery stopped at offset %zu: truncated payload_len",
                        (size_t)offset);
            free(topic);
            break;
        }
        offset += 4;

        /* 4. Read payload bytes (if payload_len > 0) */
        void* payload = nullptr;
        if (payload_len > 0) {
            payload = malloc(payload_len);
            if (!payload) {
                CSILK_LOG_E("MQ: WAL recovery memory allocation failed for payload "
                            "at offset %zu",
                            (size_t)offset);
                free(topic);
                break;
            }
            buf = csilk_io_buf_init(payload, payload_len);
            nread = csilk_io_fs_read(mq->loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
            csilk_io_fs_req_cleanup(&read_req);
            if (nread < (int)payload_len) {
                CSILK_LOG_W("MQ: WAL recovery stopped at offset %zu: truncated "
                            "payload bytes",
                            (size_t)offset);
                free(topic);
                free(payload);
                break;
            }
            offset += payload_len;
        }

        /* 5. Read stored checksum (4 bytes) */
        buf = csilk_io_buf_init((char*)&checksum, 4);
        nread = csilk_io_fs_read(mq->loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
        csilk_io_fs_req_cleanup(&read_req);
        if (nread < 4) {
            CSILK_LOG_W("MQ: WAL recovery stopped at offset %zu: truncated checksum",
                        (size_t)offset);
            free(topic);
            free(payload);
            break;
        }
        offset += 4;

        /* 6. Compute XOR checksum over topic + payload */
        uint32_t calc_checksum = 0;
        for (uint32_t i = 0; i < topic_len; i++) {
            calc_checksum ^= (uint8_t)topic[i];
        }
        const uint8_t* p = (const uint8_t*)payload;
        if (p) {
            for (uint32_t i = 0; i < payload_len; i++) {
                calc_checksum ^= p[i];
            }
        }

        /* 7. Validate: enqueue if checksum matches, stop if not */
        if (calc_checksum == checksum) {
            CSILK_LOG_D(
                "MQ: WAL recovering valid frame. Topic: '%s', Length: %u", topic, payload_len);
            _mq_enqueue(mq, topic, payload, payload_len);
            recovered_count++;
        } else {
            CSILK_LOG_W("MQ: WAL recovery checksum mismatch at offset %zu: calculated "
                        "0x%08x, stored 0x%08x. Stopping recovery.",
                        (size_t)(offset - 4),
                        calc_checksum,
                        checksum);
            free(topic);
            free(payload);
            break; /* corruption boundary — cannot find next frame */
        }

        /* 8. Free per-frame allocations (payload is deep-copied
         *    by _mq_enqueue, so we can free the originals here) */
        free(topic);
        free(payload);
    }

    CSILK_LOG_I("MQ: WAL recovery finished. Total recovered messages: %zu", recovered_count);
    return 0;
}

/** @brief Enable or switch WAL persistence for an MQ instance.
 *
 * Opens (or re-opens) a WAL file at the given path. If the MQ already has
 * an open WAL, the old one is closed first. After opening, runs
 * _mq_recovery() to replay any persisted messages from disk into the
 * in-memory queue.
 *
 * ## IDEMPOTENT — calling again switches the WAL path
 * If csilk_mq_set_persistence() is called on an MQ that already has WAL
 * enabled, the previous WAL file is closed (without truncation) and the
 * new path is opened. Any messages already in memory from the previous
 * WAL remain active — they are not written to the new file.
 *
 * ## Call chain
 * ```
 * Application                  MQ module
 *    │                            │
 *    ├─ csilk_mq_new()            │  MQ created, wal_fd = -1
 *    │                            │
 *    ├─ csilk_mq_set_persistence(──┤  enable WAL
 *    │   mq, "queue.wal")          ├─ csilk_io_fs_open(O_CREAT|O_RDWR|O_APPEND)
 *    │                             ├─ _mq_recovery()
 *    │                             │    └─ csilk_io_fs_read()  ← replay frames
 *    │                             │    └─ _mq_enqueue() ← restore to memory
 *    │                             └─ return 0
 *    │                            │
 *    ├─ csilk_mq_publish(─────────┤  publish messages
 *    │   mq, "evt", data, 12)     ├─ _mq_append_wal()  ← durable write
 *    │                             └─ _mq_enqueue()    ← in-memory queue
 * ```
 *
 * @param mq       The MQ instance (must not be nullptr).
 * @param wal_path Filesystem path to the WAL file (must not be nullptr).
 * @return 0 on success, or a negative csilk_io_fs_open error code on failure.
 * @threadsafe Serialized via wal_mutex.
 * @note The WAL file is opened with O_CREAT | O_RDWR | O_APPEND, mode 0644.
 *       An empty WAL is valid (no messages to recover). */
int
csilk_mq_set_persistence(csilk_mq_t* mq, const char* wal_path)
{
    if (!mq || !wal_path) {
        CSILK_LOG_E("MQ: Set persistence failed: invalid arguments");
        return -1;
    }

    CSILK_LOG_I("MQ: Enabling WAL persistence at path '%s'", wal_path);

    csilk_mutex_lock(&mq->wal_mutex);

    if (mq->wal_fd >= 0) {
        CSILK_LOG_I("MQ: Closing active WAL file descriptor: %d", mq->wal_fd);
        csilk_io_fs_t close_req;
        csilk_io_fs_close(mq->loop, &close_req, mq->wal_fd, nullptr);
        csilk_io_fs_req_cleanup(&close_req);
        mq->wal_fd = -1;
    }
    if (mq->wal_path) {
        free(mq->wal_path);
        mq->wal_path = nullptr;
    }

    csilk_io_fs_t open_req;
    int           fd =
        csilk_io_fs_open(mq->loop, &open_req, wal_path, O_CREAT | O_RDWR | O_APPEND, 0644, nullptr);
    csilk_io_fs_req_cleanup(&open_req);

    if (fd < 0) {
        CSILK_LOG_E("MQ: Failed to open WAL file '%s' (error: %d)", wal_path, fd);
        csilk_mutex_unlock(&mq->wal_mutex);
        return fd;
    }

    mq->wal_fd = fd;
    mq->wal_path = strdup(wal_path);

    CSILK_LOG_D("MQ: WAL file opened successfully (fd: %d). Starting replay...", fd);
    _mq_recovery(mq);

    csilk_mutex_unlock(&mq->wal_mutex);

    return 0;
}
