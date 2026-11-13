/**
 * @file wal_common.c
 * @brief Shared WAL durability, CRC32 checksum, and atomic file-write primitives.
 * @copyright MIT License
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "csilk/core/sync.h"
#include "raft_internal.h"

static uint32_t     s_crc32_table[256];
static csilk_once_t s_crc32_once = CSILK_ONCE_INIT;

static void
wal_crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256U; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) {
            if (c & 1U) {
                c = 0xEDB88320U ^ (c >> 1U);
            } else {
                c >>= 1U;
            }
        }
        s_crc32_table[i] = c;
    }
}

uint32_t
_csilk_wal_crc32(uint32_t crc, const void* data, size_t len)
{
    csilk_once(&s_crc32_once, wal_crc32_init_table);

    const uint8_t* p = (const uint8_t*)data;
    if (!p || len == 0) {
        return crc;
    }

    uint32_t c = crc ^ 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        c = s_crc32_table[(c ^ p[i]) & 0xFFU] ^ (c >> 8U);
    }
    return c ^ 0xFFFFFFFFU;
}

int
_csilk_wal_write_all(int fd, const void* buf, size_t len)
{
    if (fd < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!buf) {
        return -1;
    }

    const uint8_t* p = (const uint8_t*)buf;
    size_t         written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

int
_csilk_wal_read_all(int fd, void* buf, size_t len)
{
    if (fd < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!buf) {
        return -1;
    }

    uint8_t* p = (uint8_t*)buf;
    size_t   total = 0;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return (total == 0) ? 1 : -1;
        }
        total += (size_t)n;
    }
    return 0;
}

int
_csilk_wal_fsync_fd(int fd)
{
    if (fd < 0) {
        return -1;
    }
    int rc;
    do {
#ifdef __APPLE__
        rc = fsync(fd);
#else
        rc = fdatasync(fd);
#endif
    } while (rc < 0 && errno == EINTR);
    return rc == 0 ? 0 : -1;
}

int
_csilk_wal_fsync_dir(const char* file_path)
{
    if (!file_path) {
        return -1;
    }
    char* copy = strdup(file_path);
    if (!copy) {
        return -1;
    }
    const char* dir = dirname(copy);
    int         dfd = open(dir, O_RDONLY);
    int         rc = 0;
    if (dfd >= 0) {
        while (fsync(dfd) < 0 && errno == EINTR) {
        }
        close(dfd);
    } else {
        rc = -1;
    }
    free(copy);
    return rc;
}

int
_csilk_wal_mkdir_p(const char* dir)
{
    if (!dir || dir[0] == '\0') {
        return -1;
    }

    char* tmp = strdup(dir);
    if (!tmp) {
        return -1;
    }

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

int
_csilk_wal_write_atomic(const char* path, const void* data, size_t len)
{
    if (!path || (!data && len > 0)) {
        return -1;
    }

    size_t tmp_cap = strlen(path) + 16U;
    char*  tmp_path = malloc(tmp_cap);
    if (!tmp_path) {
        return -1;
    }
    snprintf(tmp_path, tmp_cap, "%s.tmp", path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(tmp_path);
        return -1;
    }

    if (_csilk_wal_write_all(fd, data, len) != 0) {
        close(fd);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    if (close(fd) != 0) {
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    (void)_csilk_wal_fsync_dir(path);
    free(tmp_path);
    return 0;
}

uint64_t
_csilk_raft_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}
