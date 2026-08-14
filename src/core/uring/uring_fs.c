/**
 * @file uring_fs.c
 * @brief Filesystem I/O helpers for the io_uring backend.
 *
 * Provides a zero-copy sendfile path used to stream a file descriptor's
 * contents to a socket, chunked to keep the TCP window well-fed and to avoid
 * blocking on a full socket buffer. Degrades to a stub that returns -1 on
 * platforms without sendfile(2).
 *
 * @copyright MIT License
 */

#include "csilk/core/sys_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

/**
 * @brief Send file contents to a socket using sendfile, with a completion cb.
 * @param[in] loop       Event loop (unused; kept for API symmetry).
 * @param[in] req        FS request; its result field receives bytes sent.
 * @param[in] out_fd     Destination socket/file descriptor.
 * @param[in] in_fd      Source file descriptor to read from.
 * @param[in] in_offset  Starting byte offset within in_fd.
 * @param[in] length     Total number of bytes to transfer.
 * @param[in] cb         Optional callback invoked with req on success/zero-len.
 * @return 0 if at least one byte was sent and the callback (if any) fired, -1
 *         on error or zero progress; length==0 short-circuits to 0.
 * @note Transfers in 2 MB chunks and retries on EAGAIN/EINTR. On Linux uses
 *       sendfile(2); on other platforms always returns -1.
 */
int
csilk_io_fs_sendfile(csilk_io_loop_t* loop,
                     csilk_io_fs_t*   req,
                     int              out_fd,
                     int              in_fd,
                     int64_t          in_offset,
                     size_t           length,
                     void*            cb)
{
    (void)loop;
#ifdef __linux__
    if (length == 0) {
        req->result = 0;
        if (cb) {
            void (*callback)(csilk_io_fs_t*) = (void (*)(csilk_io_fs_t*))cb;
            callback(req);
        }
        return 0;
    }

    off_t        offset = in_offset;
    ssize_t      total_sent = 0;
    size_t       remaining = length;
    const size_t CHUNK_SIZE = 2 * 1024 * 1024; // 2MB chunking for optimal TCP windowing

    while (remaining > 0) {
        size_t  to_send = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        ssize_t sent = sendfile(out_fd, in_fd, &offset, to_send);
        if (sent > 0) {
            total_sent += sent;
            remaining -= (size_t)sent;
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                /* Non-blocking socket buffer full or interrupted, yield and continue */
                continue;
            }
            break;
        } else {
            /* EOF reached prematurely */
            break;
        }
    }

    req->result = total_sent;
    if (total_sent > 0) {
        if (cb) {
            void (*callback)(csilk_io_fs_t*) = (void (*)(csilk_io_fs_t*))cb;
            callback(req);
        }
        return 0;
    }
    return -1;
#else
    (void)req;
    (void)out_fd;
    (void)in_fd;
    (void)in_offset;
    (void)length;
    (void)cb;
    return -1;
#endif
}
