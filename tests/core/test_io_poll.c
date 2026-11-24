#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"

static int s_poll_cb_called = 0;

static void
on_poll_event(csilk_io_poll_t* handle, int status, int events)
{
    (void)status;
    if (events & CSILK_IO_READABLE) {
        s_poll_cb_called++;
        csilk_io_os_fd_t fd = -1;
        int              frc = csilk_io_fileno((const csilk_io_handle_t*)handle, &fd);
        assert(frc == 0 && fd >= 0);
        char    c;
        ssize_t n = read(fd, &c, 1);
        (void)n;
        csilk_io_poll_stop(handle);
    }
}

static void
on_poll_closed(csilk_io_handle_t* handle)
{
    (void)handle;
}

int
main(void)
{
    csilk_io_loop_t* loop = csilk_io_default_loop();
    int              fds[2];
    assert(pipe(fds) == 0);

    csilk_io_poll_t poll_handle;
    int             rc = csilk_io_poll_init(loop, &poll_handle, fds[0]);
    assert(rc == 0);

    rc = csilk_io_poll_start(&poll_handle, CSILK_IO_READABLE, on_poll_event);
    assert(rc == 0);

    assert(write(fds[1], "x", 1) == 1);
    csilk_io_run(loop, CSILK_IO_RUN_ONCE);

    assert(s_poll_cb_called == 1);

    csilk_io_close((csilk_io_handle_t*)&poll_handle, on_poll_closed);
    csilk_io_run(loop, CSILK_IO_RUN_ONCE);

    close(fds[0]);
    close(fds[1]);
    printf("test_io_poll: PASS\n");
    return 0;
}
