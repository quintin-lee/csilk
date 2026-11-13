#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/uring/io_perf.h"

int csilk_uring_prep_writev_response(void*       sqe_ptr,
                                     int         fd,
                                     void*       iov_buf,
                                     const char* status_line,
                                     const char* headers,
                                     const char* body,
                                     size_t      body_len);

#ifdef CSILK_USE_URING
#include <liburing.h>
#include <sys/uio.h>
#include <unistd.h>

static void
test_uring_vector_invalid_args(void)
{
    struct io_uring_sqe dummy_sqe;
    struct iovec        iov[3];

    assert(csilk_uring_prep_writev_response(NULL, 1, iov, NULL, NULL, NULL, 0) == -1);
    assert(csilk_uring_prep_writev_response(&dummy_sqe, -1, iov, NULL, NULL, NULL, 0) == -1);
    assert(csilk_uring_prep_writev_response(&dummy_sqe, 1, NULL, NULL, NULL, NULL, 0) == -1);
    printf("test_uring_vector_invalid_args passed\n");
}

static void
test_uring_vector_pipe_writev(void)
{
    struct io_uring ring;
    int             rc = io_uring_queue_init(16, &ring, 0);
    if (rc < 0) {
        printf("test_uring_vector_pipe_writev skipped (io_uring_queue_init=%d)\n", rc);
        return;
    }

    int fds[2];
    assert(pipe(fds) == 0);

    const char*          status = "HTTP/1.1 200 OK\r\n";
    const char*          headers = "Content-Length: 5\r\n\r\n";
    const char*          body = "hello";
    const char*          expected = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    struct iovec         iov[3];
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    assert(sqe != NULL);

    rc = csilk_uring_prep_writev_response(sqe, fds[1], iov, status, headers, body, 5);
    assert(rc == 0);

    rc = io_uring_submit(&ring);
    assert(rc >= 1);

    struct io_uring_cqe* cqe = NULL;
    rc = io_uring_wait_cqe(&ring, &cqe);
    assert(rc == 0);
    assert(cqe != NULL);
    assert(cqe->res == (int)strlen(expected));
    io_uring_cqe_seen(&ring, cqe);

    char    buf[128] = {0};
    ssize_t nread = read(fds[0], buf, sizeof(buf) - 1);
    assert(nread == (ssize_t)strlen(expected));
    assert(strcmp(buf, expected) == 0);

    /* Exercise default status_line / headers / NULL body path */
    sqe = io_uring_get_sqe(&ring);
    assert(sqe != NULL);
    rc = csilk_uring_prep_writev_response(sqe, fds[1], iov, NULL, NULL, NULL, 123);
    assert(rc == 0);
    rc = io_uring_submit(&ring);
    assert(rc >= 1);
    cqe = NULL;
    rc = io_uring_wait_cqe(&ring, &cqe);
    assert(rc == 0);
    assert(cqe != NULL);
    assert(cqe->res == (int)strlen("HTTP/1.1 200 OK\r\n"));
    io_uring_cqe_seen(&ring, cqe);

    memset(buf, 0, sizeof(buf));
    nread = read(fds[0], buf, sizeof(buf) - 1);
    assert(nread == (ssize_t)strlen("HTTP/1.1 200 OK\r\n"));
    assert(strcmp(buf, "HTTP/1.1 200 OK\r\n") == 0);

    close(fds[0]);
    close(fds[1]);
    io_uring_queue_exit(&ring);
    printf("test_uring_vector_pipe_writev passed\n");
}
#else
static void
test_uring_vector_stub(void)
{
    int dummy = 0;
    assert(csilk_uring_prep_writev_response(NULL, -1, NULL, NULL, NULL, NULL, 0) == -1);
    assert(csilk_uring_prep_writev_response(&dummy, 1, &dummy, "s", "h", "b", 1) == -1);
    printf("test_uring_vector_stub passed\n");
}
#endif

int
main(void)
{
#ifdef CSILK_USE_URING
    test_uring_vector_invalid_args();
    test_uring_vector_pipe_writev();
#else
    test_uring_vector_stub();
#endif
    printf("All test_uring_vector tests passed!\n");
    return 0;
}
