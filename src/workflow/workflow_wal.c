/**
 * @file workflow_wal.c
 * @brief Binary Write-Ahead Log implementation for AI workflows.
 *
 * Architecture: Crash-safe durability for workflow execution state.
 * Events (node start, node finish, workflow end) are written as
 * fixed-size headers + variable-length payloads to a binary WAL file.
 * Each record is fsynced before returning to guarantee durability.
 *
 * The WAL is replayed during csilk_wf_resume() to reconstruct the
 * workflow's execution state after a crash or restart.
 *
 * Wire format:
 *   [magic:4 bytes][type:1 byte][timestamp:4 bytes][payload_len:4 bytes]
 *   [payload:payload_len bytes]
 *
 * @copyright MIT License
 */

#include "csilk/app/workflow_wal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/** @brief Append an event record to the workflow Write-Ahead Log.
 *
 * Algorithm:
 * 1. Open (or create) the WAL file in O_WRONLY | O_APPEND mode.
 * 2. Write a fixed-size csilk_wf_wal_header_t (magic, type, timestamp,
 *    payload_len).
 * 3. If payload_len > 0 and payload is non-nullptr, write the payload
 *    bytes immediately after the header.
 * 4. fdatasync() the file to flush the data to disk before closing.
 *
 * @param wal_path Absolute path to the WAL file.
 * @param type     Event type (e.g., WF_EV_START, WF_EV_NODE_FINISH).
 * @param payload  Opaque payload data to persist (may be nullptr if len is 0).
 * @param len      Number of payload bytes.
 * @return 0 on success, -1 if wal_path is nullptr or any write fails.
 * @note Not thread-safe for concurrent writes to the same file —
 *       callers should serialize access at the workflow context level. */
int
_wf_wal_append(const char* wal_path, csilk_wf_event_type_t type, const void* payload, size_t len)
{
	if (!wal_path) {
		return -1;
	}

	int fd = open(wal_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd < 0) {
		return -1;
	}

	csilk_wf_wal_header_t header;
	header.magic = CSILK_WF_MAGIC;
	header.type = (uint8_t)type;
	header.timestamp = (uint32_t)time(nullptr);
	header.payload_len = (uint32_t)len;

	if (write(fd, &header, sizeof(header)) != sizeof(header)) {
		close(fd);
		return -1;
	}

	if (len > 0 && payload) {
		if (write(fd, payload, len) != (ssize_t)len) {
			close(fd);
			return -1;
		}
	}

#ifdef __APPLE__
	fsync(fd);
#else
	fdatasync(fd);
#endif
	close(fd);
	return 0;
}
