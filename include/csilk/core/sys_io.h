#ifndef CSILK_SYS_IO_H
#define CSILK_SYS_IO_H

#ifdef CSILK_USE_URING

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

typedef struct {
	char* base;
	size_t len;
} csilk_io_buf_t;

static inline csilk_io_buf_t
csilk_io_buf_init(char* base, unsigned int len)
{
	csilk_io_buf_t buf;
	buf.base = base;
	buf.len = len;
	return buf;
}

typedef struct io_uring csilk_io_loop_t;

typedef struct csilk_io_handle_s {
	void* data;
	csilk_io_loop_t* loop;
} csilk_io_handle_t;

typedef struct {
	void* data;
	csilk_io_loop_t* loop;
	int fd;
} csilk_io_tcp_t;

typedef struct csilk_io_write_req {
	void* data;
	void* cb;
	csilk_io_tcp_t* handle;
} csilk_io_write_t;

typedef void (*csilk_io_write_cb)(csilk_io_write_t* req, int status);

typedef csilk_io_tcp_t csilk_io_stream_t;
typedef void (*csilk_io_close_cb)(csilk_io_handle_t* handle);
void csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb);

typedef int csilk_io_os_fd_t;
typedef int csilk_io_file_t;

static inline const char*
csilk_io_strerror(int err)
{
	return "liburing error";
}
int csilk_io_is_closing(const csilk_io_handle_t* handle);
int csilk_io_fileno(const csilk_io_handle_t* handle, csilk_io_os_fd_t* fd);

int csilk_io_write(csilk_io_write_t* req,
		   csilk_io_stream_t* handle,
		   const csilk_io_buf_t bufs[],
		   unsigned int nbufs,
		   csilk_io_write_cb cb);

// Forward declaration so csilk_io_timer_cb can use the pointer type.
typedef struct csilk_io_timer_s csilk_io_timer_t;

// Timer callback type (similar to uv_timer_cb)
typedef void (*csilk_io_timer_cb)(csilk_io_timer_t* handle);

struct csilk_io_timer_s {
	int fd;
	void* data;
	struct io_uring* ring; /**< io_uring ring for creating timeout SQEs. */
	csilk_io_timer_cb cb;  /**< Timer callback (set by csilk_io_timer_start). */
	uint8_t generation;    /**< Incremented each start to detect stale CQEs. */
};

/* --- Forward declarations --- */
typedef struct csilk_io_async_s csilk_io_async_t;
typedef void (*csilk_io_async_cb)(csilk_io_async_t* handle);

struct csilk_io_async_s {
	int event_fd;
	void* data;
	csilk_io_async_cb cb;
};

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

#include <time.h>
#include <unistd.h>
static inline uint64_t
csilk_io_hrtime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static inline void
csilk_io_sleep(unsigned int ms)
{
	usleep(ms * 1000);
}

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

#define csilk_io_hrtime uv_hrtime
#define csilk_io_sleep uv_sleep

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
static inline int
csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb)
{
	(void)loop;
	async->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	async->cb = async_cb;
	return 0;
}
static inline int
csilk_io_async_send(csilk_io_async_t* async)
{
	uint64_t val = 1;
	if (async && async->event_fd >= 0) {
		write(async->event_fd, &val, sizeof(val));
	}
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

extern int _csilk_uring_queue_work(csilk_io_work_t* req,
				   csilk_io_work_cb work_cb,
				   csilk_io_after_work_cb after_work_cb);

static inline int
csilk_io_queue_work(csilk_io_loop_t* loop,
		    csilk_io_work_t* req,
		    csilk_io_work_cb work_cb,
		    csilk_io_after_work_cb after_work_cb)
{
	(void)loop;
	return _csilk_uring_queue_work(req, work_cb, after_work_cb);
}
#endif

#ifndef CSILK_USE_URING
typedef uv_fs_t csilk_io_fs_t;

typedef uv_file csilk_io_file_t;

#define csilk_io_close uv_close
typedef uv_handle_t csilk_io_handle_t;
typedef uv_close_cb csilk_io_close_cb;

#define csilk_io_resident_set_memory uv_resident_set_memory
typedef uv_rusage_t csilk_io_rusage_t;
#define csilk_io_getrusage uv_getrusage
#define csilk_io_strerror uv_strerror

#define csilk_io_is_closing uv_is_closing
#define csilk_io_fileno uv_fileno
typedef uv_os_fd_t csilk_io_os_fd_t;

#define csilk_io_write uv_write
typedef uv_write_t csilk_io_write_t;
typedef uv_stream_t csilk_io_stream_t;
typedef uv_write_cb csilk_io_write_cb;

#define csilk_io_buf_t uv_buf_t
#define csilk_io_buf_init uv_buf_init

#define csilk_io_stat_t uv_stat_t
#define csilk_io_fs_fstat uv_fs_fstat

#define csilk_io_fs_open uv_fs_open
#define csilk_io_fs_close uv_fs_close
#define csilk_io_fs_read uv_fs_read
#define csilk_io_fs_write uv_fs_write
#define csilk_io_fs_fsync uv_fs_fsync
#define csilk_io_fs_sendfile uv_fs_sendfile
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
typedef struct {
	ssize_t result;
	struct stat statbuf;
	void* data;
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
		 const csilk_io_buf_t bufs[],
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
		  const csilk_io_buf_t bufs[],
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

#include <sys/stat.h>
typedef struct stat csilk_io_stat_t;

static inline int
csilk_io_fs_fstat(csilk_io_loop_t* loop, csilk_io_fs_t* req, int fd, void* cb)
{
	req->result = fstat(fd, &req->statbuf);
	return req->result;
}

static inline int
csilk_io_fs_sendfile(csilk_io_loop_t* loop,
		     csilk_io_fs_t* req,
		     int out_fd,
		     int in_fd,
		     int64_t in_offset,
		     size_t length,
		     void* cb)
{
#ifdef __linux__
	off_t offset = in_offset;
	req->result = sendfile(out_fd, in_fd, &offset, length);
	if (req->result >= 0) {
		if (cb) {
			void (*callback)(csilk_io_fs_t*) = (void (*)(csilk_io_fs_t*))cb;
			callback(req);
		}
		return 0;
	} else {
		return -1;
	}
#else
	return -1;
#endif
}

#endif

#ifndef CSILK_USE_URING
#define csilk_io_fs_req_cleanup uv_fs_req_cleanup
#else
static inline void
csilk_io_fs_req_cleanup(csilk_io_fs_t* req)
{
}
#endif

#ifndef CSILK_USE_URING
#define csilk_io_default_loop uv_default_loop
#define csilk_io_resident_set_memory uv_resident_set_memory
typedef uv_rusage_t csilk_io_rusage_t;
#define csilk_io_getrusage uv_getrusage
#else
static inline csilk_io_loop_t*
csilk_io_default_loop(void)
{
	return NULL;
}
#include <sys/resource.h>
typedef struct rusage csilk_io_rusage_t;
static inline int
csilk_io_resident_set_memory(size_t* rss)
{
	*rss = 0;
	return 0;
}
static inline int
csilk_io_getrusage(csilk_io_rusage_t* rusage)
{
	return getrusage(RUSAGE_SELF, rusage);
}
#endif

#endif /* CSILK_SYS_IO_H */
