#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/io_perf.h"

#ifdef CSILK_USE_URING
#include <liburing.h>
#include <sys/uio.h>

int
csilk_uring_prep_writev_response(void*       sqe_ptr,
                                 int         fd,
                                 const char* status_line,
                                 const char* headers,
                                 const char* body,
                                 size_t      body_len)
{
    if (!sqe_ptr || fd < 0) {
        return -1;
    }

    struct io_uring_sqe* sqe = (struct io_uring_sqe*)sqe_ptr;
    struct iovec*        iov = (struct iovec*)calloc(3, sizeof(struct iovec));
    if (!iov) {
        return -1;
    }

    iov[0].iov_base = (void*)(status_line ? status_line : "HTTP/1.1 200 OK\r\n");
    iov[0].iov_len = strlen((const char*)iov[0].iov_base);

    iov[1].iov_base = (void*)(headers ? headers : "");
    iov[1].iov_len = strlen((const char*)iov[1].iov_base);

    iov[2].iov_base = (void*)(body ? body : "");
    iov[2].iov_len = body_len;

    io_uring_prep_writev2(sqe, fd, iov, 3, 0, 0);
    return 0;
}
#else
int
csilk_uring_prep_writev_response(void*       sqe_ptr,
                                 int         fd,
                                 const char* status_line,
                                 const char* headers,
                                 const char* body,
                                 size_t      body_len)
{
    (void)sqe_ptr;
    (void)fd;
    (void)status_line;
    (void)headers;
    (void)body;
    (void)body_len;
    return -1;
}
#endif
