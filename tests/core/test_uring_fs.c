/**
 * @file test_uring_fs.c
 * @brief Tests for io_uring filesystem I/O helpers (sendfile, fs event).
 *
 * Covers: csilk_io_fs_sendfile zero-length short-circuit,
 *         csilk_io_fs_event_init/start/stop null-safety and lifecycle.
 * Requires CSILK_USE_URING.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

/* ---- csilk_io_fs_sendfile ---- */

static void
test_sendfile_null_req(void)
{
    printf("Testing csilk_io_fs_sendfile with NULL req...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);
    int rc = csilk_io_fs_sendfile(&loop, NULL, 0, 0, 0, 0, NULL);
    csilk_io_loop_close(&loop);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_sendfile_zero_length(void)
{
    printf("Testing csilk_io_fs_sendfile zero-length short-circuit...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    csilk_io_fs_t req;
    memset(&req, 0, sizeof(req));

    int rc = csilk_io_fs_sendfile(&loop, &req, -1, -1, 0, 0, NULL);
    csilk_io_loop_close(&loop);
    assert(rc == 0);
    assert(req.result == 0);
    printf("  passed\n");
}

static void
test_sendfile_zero_length_with_callback(void)
{
    printf("Testing csilk_io_fs_sendfile zero-length with callback...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    csilk_io_fs_t req;
    memset(&req, 0, sizeof(req));
    int cb_called = 0;

    void cb(csilk_io_fs_t * r)
    {
        (void)r;
        cb_called = 1;
    }

    int rc = csilk_io_fs_sendfile(&loop, &req, -1, -1, 0, 0, (void*)cb);
    csilk_io_loop_close(&loop);
    assert(rc == 0);
    assert(cb_called == 1);
    printf("  passed\n");
}

/* ---- csilk_io_fs_event ---- */

static void
test_fs_event_init_null(void)
{
    printf("Testing csilk_io_fs_event_init with NULL handle...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);
    int rc = csilk_io_fs_event_init(&loop, NULL);
    csilk_io_loop_close(&loop);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_fs_event_init_ok(void)
{
    printf("Testing csilk_io_fs_event_init success...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    csilk_io_fs_event_t handle;
    int                 rc = csilk_io_fs_event_init(&loop, &handle);
    assert(rc == 0);
    assert(handle.type == CSILK_IO_HANDLE_FS_EVENT);
    assert(handle.loop == &loop);
    csilk_io_loop_close(&loop);
    printf("  passed\n");
}

static void
test_fs_event_start_null(void)
{
    printf("Testing csilk_io_fs_event_start with NULL handle...\n");
    csilk_io_fs_event_cb cb = NULL;
    int                  rc = csilk_io_fs_event_start(NULL, cb, "/tmp/test", 0);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_fs_event_start_null_path(void)
{
    printf("Testing csilk_io_fs_event_start with NULL path...\n");
    csilk_io_fs_event_t handle;
    memset(&handle, 0, sizeof(handle));
    int rc = csilk_io_fs_event_start(&handle, NULL, NULL, 0);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_fs_event_start_stop(void)
{
    printf("Testing csilk_io_fs_event_start / stop lifecycle...\n");
    csilk_io_fs_event_t handle;
    memset(&handle, 0, sizeof(handle));

    int rc = csilk_io_fs_event_start(&handle, NULL, "/tmp/csilk_test_path", 0);
    assert(rc == 0);
    assert(handle.path != NULL);

    rc = csilk_io_fs_event_stop(&handle);
    assert(rc == 0);
    assert(handle.path == NULL);
    assert(handle.cb == NULL);
    printf("  passed\n");
}

static void
test_fs_event_stop_null(void)
{
    printf("Testing csilk_io_fs_event_stop with NULL handle...\n");
    int rc = csilk_io_fs_event_stop(NULL);
    assert(rc == -1);
    printf("  passed\n");
}

#endif /* CSILK_USE_URING */

int
main(void)
{
#ifdef CSILK_USE_URING
    test_sendfile_null_req();
    test_sendfile_zero_length();
    test_sendfile_zero_length_with_callback();
    test_fs_event_init_null();
    test_fs_event_init_ok();
    test_fs_event_start_null();
    test_fs_event_start_null_path();
    test_fs_event_start_stop();
    test_fs_event_stop_null();
#else
    printf("Skipping test_uring_fs: CSILK_USE_URING not enabled\n");
#endif

    printf("All test_uring_fs tests passed successfully!\n");
    return 0;
}
