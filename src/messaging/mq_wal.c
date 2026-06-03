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

#include "csilk/core/mq_types.h"
#include "csilk/mq.h"

#include "mq_internal.h"

int
_mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	if (!mq || mq->wal_fd < 0 || !topic) {
		return 0;
	}

	uv_mutex_lock(&mq->wal_mutex);

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

	uv_buf_t bufs[5];
	bufs[0] = uv_buf_init((char*)&topic_len, 4);
	bufs[1] = uv_buf_init((char*)topic, topic_len);
	bufs[2] = uv_buf_init((char*)&payload_len, 4);
	bufs[3] = uv_buf_init((char*)payload, payload_len);
	bufs[4] = uv_buf_init((char*)&checksum, 4);

	uv_fs_t write_req;
	int result =
	    uv_fs_write(mq->async_handle.loop, &write_req, mq->wal_fd, bufs, 5, -1, nullptr);
	uv_fs_req_cleanup(&write_req);

	if (result >= 0) {
		uv_fs_t sync_req;
		uv_fs_fsync(mq->async_handle.loop, &sync_req, mq->wal_fd, nullptr);
		uv_fs_req_cleanup(&sync_req);
	}

	uv_mutex_unlock(&mq->wal_mutex);
	return (result >= 0) ? 0 : -1;
}

int
_mq_recovery(csilk_mq_t* mq)
{
	if (!mq || mq->wal_fd < 0) {
		return 0;
	}

	uint64_t offset = 0;
	while (1) {
		uint32_t topic_len = 0;
		uint32_t payload_len = 0;
		uint32_t checksum = 0;
		uv_fs_t read_req;
		int nread;

		uv_buf_t buf = uv_buf_init((char*)&topic_len, 4);
		nread = uv_fs_read(
		    mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
		uv_fs_req_cleanup(&read_req);
		if (nread < 4) {
			break;
		}
		offset += 4;

		char* topic = malloc(topic_len + 1);
		if (!topic) {
			break;
		}
		buf = uv_buf_init(topic, topic_len);
		nread = uv_fs_read(
		    mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
		uv_fs_req_cleanup(&read_req);
		if (nread < (int)topic_len) {
			free(topic);
			break;
		}
		topic[topic_len] = '\0';
		offset += topic_len;

		buf = uv_buf_init((char*)&payload_len, 4);
		nread = uv_fs_read(
		    mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
		uv_fs_req_cleanup(&read_req);
		if (nread < 4) {
			free(topic);
			break;
		}
		offset += 4;

		void* payload = nullptr;
		if (payload_len > 0) {
			payload = malloc(payload_len);
			if (!payload) {
				free(topic);
				break;
			}
			buf = uv_buf_init(payload, payload_len);
			nread = uv_fs_read(
			    mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
			uv_fs_req_cleanup(&read_req);
			if (nread < (int)payload_len) {
				free(topic);
				free(payload);
				break;
			}
			offset += payload_len;
		}

		buf = uv_buf_init((char*)&checksum, 4);
		nread = uv_fs_read(
		    mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, nullptr);
		uv_fs_req_cleanup(&read_req);
		if (nread < 4) {
			free(topic);
			free(payload);
			break;
		}
		offset += 4;

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

		if (calc_checksum == checksum) {
			_mq_enqueue(mq, topic, payload, payload_len);
		} else {
			free(topic);
			free(payload);
			break;
		}

		free(topic);
		free(payload);
	}

	return 0;
}

int
csilk_mq_set_persistence(csilk_mq_t* mq, const char* wal_path)
{
	if (!mq || !wal_path) {
		return -1;
	}

	uv_mutex_lock(&mq->wal_mutex);

	if (mq->wal_fd >= 0) {
		uv_fs_t close_req;
		uv_fs_close(mq->async_handle.loop, &close_req, mq->wal_fd, nullptr);
		uv_fs_req_cleanup(&close_req);
		mq->wal_fd = -1;
	}
	if (mq->wal_path) {
		free(mq->wal_path);
		mq->wal_path = nullptr;
	}

	uv_fs_t open_req;
	int fd = uv_fs_open(
	    mq->async_handle.loop, &open_req, wal_path, O_CREAT | O_RDWR | O_APPEND, 0644, nullptr);
	uv_fs_req_cleanup(&open_req);

	if (fd < 0) {
		uv_mutex_unlock(&mq->wal_mutex);
		return fd;
	}

	mq->wal_fd = fd;
	mq->wal_path = strdup(wal_path);

	_mq_recovery(mq);

	uv_mutex_unlock(&mq->wal_mutex);

	return 0;
}
