#include "csilk/core/sys_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    off_t     offset = in_offset;
    ssize_t   total_sent = 0;
    size_t    remaining = length;
    int       retries = 0;
    const int max_retries = 500;

    while (remaining > 0 && retries < max_retries) {
        req->result = sendfile(out_fd, in_fd, &offset, remaining);
        if (req->result > 0) {
            total_sent += req->result;
            remaining -= (size_t)req->result;
            retries = 0;
        } else if (req->result == 0) {
            usleep(100);
            retries++;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100);
                retries++;
            } else {
                break;
            }
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
