/**
 * @file wal_common.h
 * @brief Shared WAL durability, CRC32 checksum, and atomic file-write primitives.
 * @copyright MIT License
 */

#ifndef CSILK_WAL_COMMON_H
#define CSILK_WAL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

uint32_t _csilk_wal_crc32(uint32_t crc, const void* data, size_t len);
int      _csilk_wal_write_all(int fd, const void* buf, size_t len);
int      _csilk_wal_writev_all(int fd, const struct iovec* iov, int iovcnt);
int      _csilk_wal_read_all(int fd, void* buf, size_t len);
int      _csilk_wal_fsync_fd(int fd);
int      _csilk_wal_fsync_dir(const char* file_path);
int      _csilk_wal_mkdir_p(const char* dir);
int      _csilk_wal_write_atomic(const char* path, const void* data, size_t len);
uint64_t _csilk_raft_now_ms(void);

#endif /* CSILK_WAL_COMMON_H */
