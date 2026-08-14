/**
 * @file uring_vector.c
 * @brief io_uring vectorized response write preparation.
 *
 * Builds a three-segment writev (status line, headers, body) from caller owned
 * buffers and attaches it to a supplied submission queue entry. Falls back to a
 * stub that returns -1 when io_uring is not compiled in.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/io_perf.h"

#ifdef CSILK_USE_URING
#include <liburing.h>
#include <sys/uio.h>

/**
 * @brief Prepare a vectorized response write against an io_uring SQE.
 * @param[in] sqe_ptr Pointer to the io_uring_sqe to populate.
 * @param[in] fd      File descriptor (socket) to write to; must be >= 0.
 * @param[in] status_line NUL-terminated status line (defaults to
 *                        "HTTP/1.1 200 OK\r\n" if NULL).
 * @param[in] headers NUL-terminated header block (defaults to "" if NULL).
 * @param[in] body    Body bytes (defaults to "" if NULL).
 * @param[in] body_len Number of bytes in body.
 * @return 0 on success, -1 on invalid arguments or allocation failure.
 * @note Allocates a 3-element iovec array that is transferred to the kernel;
 *       callers must NOT free it. The status line and header strings are used
 *       by reference, so they must remain valid until the write completes.
 */
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

/**
 * @brief Stub for csilk_uring_prep_writev_response when io_uring is disabled.
 * @param[in] sqe_ptr Unused.
 * @param[in] fd     Unused.
 * @param[in] status_line Unused.
 * @param[in] headers Unused.
 * @param[in] body   Unused.
 * @param[in] body_len Unused.
 * @return Always -1.
 */
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
