#ifndef CSILK_MQ_INTERNAL_H
#define CSILK_MQ_INTERNAL_H

#include <stddef.h>

#include "csilk/mq.h"

/** @brief Enqueue a message into the MQ's in-memory linked list.
 *
 * Deep-copies topic and payload, appends to the queue tail under
 * queue_mutex, and signals the event loop via uv_async_send().
 *
 * @param mq      MQ instance.
 * @param topic   Topic string (deep-copied).
 * @param payload Opaque payload pointer (deep-copied; may be NULL).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 on allocation failure.
 * @threadsafe */
int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);

/** @brief Append a message frame to the WAL file on disk.
 *
 * Writes [topic_len:4][topic:N][payload_len:4][payload:M][xor:4] to
 * the WAL, then fsyncs for durability.
 *
 * @param mq      MQ instance (must have wal_fd >= 0).
 * @param topic   Topic string.
 * @param payload Opaque payload pointer (may be NULL).
 * @param len     Payload length.
 * @return 0 on success, -1 on write failure.
 * @threadsafe (uses wal_mutex). */
int _mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);

/** @brief Recover messages from the WAL file on startup.
 *
 * Reads the WAL sequentially from offset 0, validates each frame's XOR
 * checksum, and re-enqueues valid frames into the in-memory queue via
 * _mq_enqueue(). Stops at the first invalid/corrupt frame.
 *
 * @param mq MQ instance (must have wal_fd >= 0).
 * @return 0 on success.
 * @threadsafe (called under wal_mutex by csilk_mq_set_persistence). */
int _mq_recovery(csilk_mq_t* mq);

#endif
