#include <uv.h>

#ifdef CSILK_USE_URING

uv_buf_t
uv_buf_init(char* base, unsigned int len)
{
	uv_buf_t buf;
	buf.base = base;
	buf.len = len;
	return buf;
}

int
uv_write(
    uv_write_t* req, uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs, uv_write_cb cb)
{
	return -1;
}

int
uv_is_closing(const uv_handle_t* handle)
{
	return 1;
}

void
uv_close(uv_handle_t* handle, uv_close_cb close_cb)
{
	if (close_cb) {
		close_cb(handle);
	}
}

int
uv_fileno(const uv_handle_t* handle, uv_os_fd_t* fd)
{
	return -1;
}

int
uv_fs_sendfile(uv_loop_t* loop,
	       uv_fs_t* req,
	       uv_file out_fd,
	       uv_file in_fd,
	       int64_t in_offset,
	       size_t length,
	       uv_fs_cb cb)
{
	return -1;
}

const char*
uv_strerror(int err)
{
	return "libuv stripped";
}

void
uv_fs_req_cleanup(uv_fs_t* req)
{
}

#endif

/* Dummy callbacks for HTTP1 when CSILK_USE_URING is ON (un-refactored paths) */
#include "core/srv_internal.h"
#include "core/ctx_internal.h"

void
on_idle_timeout(csilk_io_timer_t* handle)
{
}

void
on_write_timeout(csilk_io_timer_t* handle)
{
}
void
on_close(uv_handle_t* handle)
{
}
void
alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
}

int
uv_fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb)
{
	return -1;
}

int
csilk_io_timer_start(csilk_io_timer_t* handle,
		     csilk_io_timer_cb cb,
		     uint64_t timeout,
		     uint64_t repeat)
{
	return -1;
}

int
csilk_io_timer_stop(csilk_io_timer_t* handle)
{
	return -1;
}

/* Also fs cleanup dummy because we need to link it */
void
on_read_timeout(csilk_io_timer_t* handle)
{
}

int
uv_fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode, uv_fs_cb cb)
{
	return -1;
}
int
uv_fs_fstat(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb)
{
	return -1;
}
uint64_t
uv_hrtime(void)
{
	return 0;
}

int
uv_queue_work(uv_loop_t* loop, uv_work_t* req, uv_work_cb work_cb, uv_after_work_cb after_work_cb)
{
	return -1;
}
void
uv_sleep(unsigned int msec)
{
}

int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
	return -1;
}
int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
	return -1;
}
int
uv_cond_init(uv_cond_t* cond)
{
	return -1;
}
void
uv_cond_signal(uv_cond_t* cond)
{
}
void
uv_cond_wait(uv_cond_t* cond, uv_mutex_t* mutex)
{
}
void
uv_cond_destroy(uv_cond_t* cond)
{
}

int
uv_resident_set_memory(size_t* rss)
{
	return -1;
}
int
uv_getrusage(uv_rusage_t* rusage)
{
	return -1;
}

int
uv_timer_init(uv_loop_t* loop, uv_timer_t* handle)
{
	return -1;
}
int
uv_timer_start(uv_timer_t* handle, uv_timer_cb cb, uint64_t timeout, uint64_t repeat)
{
	return -1;
}
int
uv_timer_stop(uv_timer_t* handle)
{
	return -1;
}
