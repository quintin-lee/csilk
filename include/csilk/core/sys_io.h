#ifndef CSILK_SYS_IO_H
#define CSILK_SYS_IO_H

#ifdef CSILK_USE_URING

#include <liburing.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct io_uring csilk_io_loop_t;

typedef struct {
	int fd;
	void* data;
} csilk_io_tcp_t;

typedef struct {
	int fd;
	void* data;
} csilk_io_timer_t;

typedef struct {
	int event_fd;
	void* data;
} csilk_io_async_t;

typedef struct {
	int signal_fd;
	void* data;
} csilk_io_signal_t;

typedef struct {
	int fd;
	void* data;
} csilk_io_fs_event_t;

typedef struct {
	void* data;
} csilk_io_work_t;

// Timer callback type (similar to uv_timer_cb)
typedef void (*csilk_io_timer_cb)(csilk_io_timer_t* handle);

typedef void (*csilk_io_fs_event_cb)(csilk_io_fs_event_t* handle,
				     const char* filename,
				     int events,
				     int status);

typedef enum { CSILK_IO_RUN_DEFAULT = 0, CSILK_IO_RUN_ONCE, CSILK_IO_RUN_NOWAIT } csilk_io_run_mode;

int csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode);

int csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle);
int csilk_io_timer_start(csilk_io_timer_t* handle,
			 csilk_io_timer_cb cb,
			 uint64_t timeout,
			 uint64_t repeat);
int csilk_io_timer_stop(csilk_io_timer_t* handle);

int csilk_io_fs_event_init(csilk_io_loop_t* loop, csilk_io_fs_event_t* handle);
int csilk_io_fs_event_start(csilk_io_fs_event_t* handle,
			    csilk_io_fs_event_cb cb,
			    const char* path,
			    unsigned int flags);
int csilk_io_fs_event_stop(csilk_io_fs_event_t* handle);

#else

#include <uv.h>
typedef uv_loop_t csilk_io_loop_t;
typedef uv_tcp_t csilk_io_tcp_t;
typedef uv_timer_t csilk_io_timer_t;
typedef uv_async_t csilk_io_async_t;
typedef uv_signal_t csilk_io_signal_t;
typedef uv_work_t csilk_io_work_t;

typedef uv_fs_event_t csilk_io_fs_event_t;
typedef uv_timer_cb csilk_io_timer_cb;
typedef uv_fs_event_cb csilk_io_fs_event_cb;

typedef uv_run_mode csilk_io_run_mode;
#define CSILK_IO_RUN_DEFAULT UV_RUN_DEFAULT
#define CSILK_IO_RUN_ONCE UV_RUN_ONCE
#define CSILK_IO_RUN_NOWAIT UV_RUN_NOWAIT

static inline int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
	return uv_run(loop, mode);
}

static inline int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
	return uv_timer_init(loop, handle);
}

static inline int
csilk_io_timer_start(csilk_io_timer_t* handle,
		     csilk_io_timer_cb cb,
		     uint64_t timeout,
		     uint64_t repeat)
{
	return uv_timer_start(handle, cb, timeout, repeat);
}

static inline int
csilk_io_timer_stop(csilk_io_timer_t* handle)
{
	return uv_timer_stop(handle);
}

static inline int
csilk_io_fs_event_init(csilk_io_loop_t* loop, csilk_io_fs_event_t* handle)
{
	return uv_fs_event_init(loop, handle);
}

static inline int
csilk_io_fs_event_start(csilk_io_fs_event_t* handle,
			csilk_io_fs_event_cb cb,
			const char* path,
			unsigned int flags)
{
	return uv_fs_event_start(handle, cb, path, flags);
}

static inline int
csilk_io_fs_event_stop(csilk_io_fs_event_t* handle)
{
	return uv_fs_event_stop(handle);
}

#endif

#ifndef CSILK_USE_URING
typedef void (*csilk_io_async_cb)(csilk_io_async_t* handle);
static inline int
csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb)
{
	return uv_async_init(loop, async, async_cb);
}
static inline int
csilk_io_async_send(csilk_io_async_t* async)
{
	return uv_async_send(async);
}
#else
typedef void (*csilk_io_async_cb)(csilk_io_async_t* handle);
static inline int
csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb)
{
	return 0;
}
static inline int
csilk_io_async_send(csilk_io_async_t* async)
{
	return 0;
}
#endif

#ifndef CSILK_USE_URING
typedef void (*csilk_io_work_cb)(csilk_io_work_t* req);
typedef void (*csilk_io_after_work_cb)(csilk_io_work_t* req, int status);
static inline int
csilk_io_queue_work(csilk_io_loop_t* loop,
		    csilk_io_work_t* req,
		    csilk_io_work_cb work_cb,
		    csilk_io_after_work_cb after_work_cb)
{
	return uv_queue_work(loop, req, work_cb, after_work_cb);
}
#else
typedef void (*csilk_io_work_cb)(csilk_io_work_t* req);
typedef void (*csilk_io_after_work_cb)(csilk_io_work_t* req, int status);
static inline int
csilk_io_queue_work(csilk_io_loop_t* loop,
		    csilk_io_work_t* req,
		    csilk_io_work_cb work_cb,
		    csilk_io_after_work_cb after_work_cb)
{
	return 0; // Not implemented yet
}
#endif

#ifndef CSILK_USE_URING
typedef uv_fs_t csilk_io_fs_t;
#define csilk_io_fs_open uv_fs_open
#define csilk_io_fs_close uv_fs_close
#define csilk_io_fs_read uv_fs_read
#define csilk_io_fs_write uv_fs_write
#define csilk_io_fs_fsync uv_fs_fsync
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
typedef struct {
	ssize_t result;
} csilk_io_fs_t;
static inline int
csilk_io_fs_open(
    csilk_io_loop_t* loop, csilk_io_fs_t* req, const char* path, int flags, int mode, void* cb)
{
	req->result = open(path, flags, mode);
	return req->result;
}
static inline int
csilk_io_fs_close(csilk_io_loop_t* loop, csilk_io_fs_t* req, int fd, void* cb)
{
	req->result = close(fd);
	return req->result;
}
static inline int
csilk_io_fs_read(csilk_io_loop_t* loop,
		 csilk_io_fs_t* req,
		 int fd,
		 const uv_buf_t bufs[],
		 unsigned int nbufs,
		 int64_t offset,
		 void* cb)
{
	req->result = preadv(fd, (const struct iovec*)bufs, nbufs, offset);
	return req->result;
}
static inline int
csilk_io_fs_write(csilk_io_loop_t* loop,
		  csilk_io_fs_t* req,
		  int fd,
		  const uv_buf_t bufs[],
		  unsigned int nbufs,
		  int64_t offset,
		  void* cb)
{
	if (offset == -1) {
		req->result = writev(fd, (const struct iovec*)bufs, nbufs);
	} else {
		req->result = pwritev(fd, (const struct iovec*)bufs, nbufs, offset);
	}
	return req->result;
}
static inline int
csilk_io_fs_fsync(csilk_io_loop_t* loop, csilk_io_fs_t* req, int fd, void* cb)
{
	req->result = fsync(fd);
	return req->result;
}
#endif

#ifndef CSILK_USE_URING
#define csilk_io_fs_req_cleanup uv_fs_req_cleanup
#else
static inline void
csilk_io_fs_req_cleanup(csilk_io_fs_t* req)
{
	(void)req;
}
#endif

#ifndef CSILK_USE_URING
#define csilk_io_default_loop uv_default_loop
#else
static inline csilk_io_loop_t*
csilk_io_default_loop(void)
{
	return NULL;
}
#endif

#endif /* CSILK_SYS_IO_H */
