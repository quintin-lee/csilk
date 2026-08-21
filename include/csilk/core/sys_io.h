#pragma once
/**
 * @file csilk/core/sys_io.h
 * @brief Portable async I/O abstraction over io_uring or libuv.
 *
 * When CSILK_USE_URING is defined, this header exposes a libuv-style API
 * implemented on top of Linux io_uring.  Otherwise it is a thin set of
 * typedefs, macros and inline wrappers around the equivalent libuv types
 * and functions, so callers can write backend-agnostic async I/O code.
 *
 * @copyright MIT License
 */

#ifdef CSILK_USE_URING

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <liburing.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

/** @brief A single I/O buffer (base pointer + length). */
typedef struct {
    char*  base; /**< Buffer base pointer. */
    size_t len;  /**< Buffer length in bytes. */
} csilk_io_buf_t;

/**
 * @brief Construct a csilk_io_buf_t from a base pointer and length.
 * @param[in] base Buffer base pointer.
 * @param[in] len Buffer length in bytes.
 * @return Initialized buffer descriptor (by value).
 */
static inline csilk_io_buf_t
csilk_io_buf_init(char* base, unsigned int len)
{
    csilk_io_buf_t buf;
    buf.base = base;
    buf.len = len;
    return buf;
}

/** @brief Loop control flags and state. */
#define CSILK_IO_HANDLE_CLOSING 0x01
#define CSILK_IO_HANDLE_ACTIVE 0x02

typedef enum {
    CSILK_IO_HANDLE_UNKNOWN = 0,
    CSILK_IO_HANDLE_TCP,
    CSILK_IO_HANDLE_TIMER,
    CSILK_IO_HANDLE_ASYNC,
    CSILK_IO_HANDLE_SIGNAL,
    CSILK_IO_HANDLE_FS_EVENT
} csilk_io_handle_type_t;

typedef struct uring_op_context_s uring_op_context_t;

/** @brief Opaque event loop type (io_uring instance with loop control). */
typedef struct csilk_io_loop_s {
    struct io_uring     ring;
    int                 stop_flag;
    int                 active_handles;
    int                 running;
    uring_op_context_t* op_pool;
    uint32_t*           op_free_stack;
    uint32_t            op_free_head;
    uint32_t            op_pool_capacity;
} csilk_io_loop_t;

typedef struct csilk_io_handle_s csilk_io_handle_t;
typedef void (*csilk_io_close_cb)(csilk_io_handle_t* handle);

#define CSILK_IO_HANDLE_FIELDS                                                                     \
    void*                  data;                                                                   \
    csilk_io_loop_t*       loop;                                                                   \
    int                    fd;                                                                     \
    int                    flags;                                                                  \
    csilk_io_handle_type_t type;                                                                   \
    csilk_io_close_cb      close_cb;                                                               \
    uint64_t               generation;

/** @brief Generic handle with per-handle user data and owning loop. */
struct csilk_io_handle_s {
    CSILK_IO_HANDLE_FIELDS
};

typedef int                   csilk_io_os_sock_t;
typedef struct csilk_io_tcp_s csilk_io_tcp_t;
typedef csilk_io_tcp_t        csilk_io_stream_t;

typedef void (*csilk_io_connection_cb)(csilk_io_stream_t* server, int status);
typedef void (*csilk_io_alloc_cb)(csilk_io_handle_t* handle,
                                  size_t             suggested_size,
                                  csilk_io_buf_t*    buf);
typedef void (*csilk_io_read_cb)(csilk_io_stream_t*    stream,
                                 ssize_t               nread,
                                 const csilk_io_buf_t* buf);

/** @brief A TCP/stream connection handle. */
struct csilk_io_tcp_s {
    CSILK_IO_HANDLE_FIELDS
    csilk_io_connection_cb connection_cb;
    csilk_io_alloc_cb      alloc_cb;
    csilk_io_read_cb       read_cb;
    int                    reading;
    csilk_io_buf_t         recv_buf;
};

/** @brief Write request passed to csilk_io_write. */
typedef struct csilk_io_write_req {
    void*           data;   /**< Opaque user data. */
    void*           cb;     /**< User callback pointer. */
    csilk_io_tcp_t* handle; /**< Stream handle the write targets. */
} csilk_io_write_t;

/**
 * @brief Write completion callback type.
 * @param[in,out] req The write request that completed.
 * @param[in] status 0 on success, or a negative error code.
 */
typedef void (*csilk_io_write_cb)(csilk_io_write_t* req, int status);

/**
 * @brief Close a handle asynchronously.
 * @param[in,out] handle Handle to close.
 * @param[in] cb Callback invoked when closing completes.
 */
void csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb);

/** @brief OS file-descriptor type (int under io_uring). */
typedef int csilk_io_os_fd_t;
/** @brief File handle type (int file descriptor under io_uring). */
typedef int csilk_io_file_t;

/**
 * @brief Return a human-readable description of an I/O error.
 * @param[in] err Error code (currently ignored).
 * @return Static string "liburing error".
 */
static inline const char*
csilk_io_strerror(int err)
{
    if (err < 0) {
        err = -err;
    }
    if (err == 0) {
        return "Success";
    }
    return strerror(err);
}
/**
 * @brief Check whether a handle is closing or closed.
 * @param[in] handle Handle to query.
 * @return Non-zero if the handle is closing, 0 otherwise.
 */
static inline int
csilk_io_is_closing(const csilk_io_handle_t* handle)
{
    return (handle && (handle->flags & CSILK_IO_HANDLE_CLOSING)) ? 1 : 0;
}

/**
 * @brief Retrieve the underlying OS file descriptor of a handle.
 * @param[in] handle Handle to query.
 * @param[out] fd Receives the file descriptor.
 * @return 0 on success, or a negative error code.
 */
static inline int
csilk_io_fileno(const csilk_io_handle_t* handle, csilk_io_os_fd_t* fd)
{
    if (!handle || !fd || handle->fd < 0) {
        return -1;
    }
    *fd = handle->fd;
    return 0;
}

/**
 * @brief Queue a write of @p nbufs buffers to a stream.
 * @param[in,out] req Write request (owned by caller until callback).
 * @param[in,out] handle Stream to write to.
 * @param[in] bufs Array of buffers to write.
 * @param[in] nbufs Number of buffers in @p bufs.
 * @param[in] cb Completion callback.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_write(csilk_io_write_t*    req,
                   csilk_io_stream_t*   handle,
                   const csilk_io_buf_t bufs[],
                   unsigned int         nbufs,
                   csilk_io_write_cb    cb);

// Forward declaration so csilk_io_timer_cb can use the pointer type.
typedef struct csilk_io_timer_s csilk_io_timer_t;

// Timer callback type (similar to uv_timer_cb)
/**
 * @brief Timer expiration callback type.
 * @param[in,out] handle The timer handle that fired.
 */
typedef void (*csilk_io_timer_cb)(csilk_io_timer_t* handle);

/** @brief Timer handle. */
struct csilk_io_timer_s {
    CSILK_IO_HANDLE_FIELDS
    csilk_io_timer_cb cb;   /**< Timer callback (set by csilk_io_timer_start). */
    uint64_t          timeout;
    uint64_t          repeat;
    struct io_uring*  ring; /**< io_uring ring (alias of &loop->ring). */
};

/* --- Forward declarations --- */
typedef struct csilk_io_async_s csilk_io_async_t;
/**
 * @brief Async wake-up callback type.
 * @param[in,out] handle The async handle that was signalled.
 */
typedef void (*csilk_io_async_cb)(csilk_io_async_t* handle);

/** @brief Async (cross-thread wake-up) handle. */
struct csilk_io_async_s {
    CSILK_IO_HANDLE_FIELDS
    csilk_io_async_cb cb; /**< Callback invoked when signalled. */
    _Atomic(int)
        event_fd; /**< eventfd used to signal the loop (alias of fd). Atomically accessed to avoid TSan data races between send/close threads. */
};

typedef struct csilk_io_signal_s csilk_io_signal_t;
typedef void (*csilk_io_signal_cb)(csilk_io_signal_t* handle, int signum);

/** @brief Signal (POSIX signal) handle. */
struct csilk_io_signal_s {
    CSILK_IO_HANDLE_FIELDS
    csilk_io_signal_cb cb;
    int                signal_fd; /**< Signal file descriptor (alias of fd). */
    int                signum;
};

typedef struct csilk_io_fs_event_s csilk_io_fs_event_t;

/**
 * @brief Filesystem event callback type.
 * @param[in,out] handle The fs-event handle.
 * @param[in] filename Name of the changed path (may be NULL).
 * @param[in] events Bitmask of events that occurred.
 * @param[in] status 0 on success, or a negative error code.
 */
typedef void (*csilk_io_fs_event_cb)(csilk_io_fs_event_t* handle,
                                     const char*          filename,
                                     int                  events,
                                     int                  status);

/** @brief Filesystem event (inotify-style) handle. */
struct csilk_io_fs_event_s {
    CSILK_IO_HANDLE_FIELDS
    csilk_io_fs_event_cb cb;
    char*                path;
};

/** @brief Generic work (thread-pool job) handle. */
typedef struct {
    void* data; /**< Opaque user data. */
} csilk_io_work_t;

#include <time.h>
#include <unistd.h>
/**
 * @brief Return the current monotonic time in nanoseconds.
 * @return Nanoseconds since an arbitrary monotonic epoch.
 */
static inline uint64_t
csilk_io_hrtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
/**
 * @brief Sleep the calling thread for @p ms milliseconds.
 * @param[in] ms Milliseconds to sleep.
 */
static inline void
csilk_io_sleep(unsigned int ms)
{
    usleep(ms * 1000);
}

/** @brief Loop run modes, controlling how long csilk_io_run blocks. */
typedef enum { CSILK_IO_RUN_DEFAULT = 0, CSILK_IO_RUN_ONCE, CSILK_IO_RUN_NOWAIT } csilk_io_run_mode;

/**
 * @brief Run the event loop until it is stopped or has no active work.
 * @param[in,out] loop Loop to run.
 * @param[in] mode Run mode (default, once, or non-blocking).
 * @return 0 when the loop exits, or a negative error code.
 */
int csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode);

/**
 * @brief Check whether the event loop has active handles or pending requests.
 */
static inline int
csilk_io_loop_alive(const csilk_io_loop_t* loop)
{
    return loop ? loop->active_handles > 0 : 0;
}

/**
 * @brief Initialize a timer handle on a loop.
 * @param[in,out] loop Loop that will own the timer.
 * @param[in,out] handle Timer handle to initialize.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle);
/**
 * @brief Start (or restart) a timer.
 * @param[in,out] handle Timer handle.
 * @param[in] cb Callback invoked on each expiration.
 * @param[in] timeout Milliseconds until the first expiration.
 * @param[in] repeat Milliseconds between subsequent expirations (0 = one-shot).
 * @return 0 on success, or a negative error code.
 */
int csilk_io_timer_start(csilk_io_timer_t* handle,
                         csilk_io_timer_cb cb,
                         uint64_t          timeout,
                         uint64_t          repeat);
/**
 * @brief Stop a timer (further expirations are suppressed).
 * @param[in,out] handle Timer handle to stop.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_timer_stop(csilk_io_timer_t* handle);

/**
 * @brief Initialize a filesystem-event handle on a loop.
 * @param[in,out] loop Loop that will own the handle.
 * @param[in,out] handle Handle to initialize.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_fs_event_init(csilk_io_loop_t* loop, csilk_io_fs_event_t* handle);
/**
 * @brief Start watching a path for filesystem events.
 * @param[in,out] handle Handle to start.
 * @param[in] cb Callback invoked on events.
 * @param[in] path Path to watch.
 * @param[in] flags Backend-specific watch flags.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_fs_event_start(csilk_io_fs_event_t* handle,
                            csilk_io_fs_event_cb cb,
                            const char*          path,
                            unsigned int         flags);
/**
 * @brief Stop watching filesystem events.
 * @param[in,out] handle Handle to stop.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_fs_event_stop(csilk_io_fs_event_t* handle);

int      csilk_io_loop_init(csilk_io_loop_t* loop);
int      csilk_io_loop_close(csilk_io_loop_t* loop);
void     csilk_io_stop(csilk_io_loop_t* loop);
uint64_t csilk_io_now(const csilk_io_loop_t* loop);
void     csilk_io_update_time(csilk_io_loop_t* loop);

static inline int
csilk_io_is_active(const csilk_io_handle_t* handle)
{
    return (handle && (handle->flags & CSILK_IO_HANDLE_ACTIVE)) ? 1 : 0;
}

int csilk_io_tcp_init(csilk_io_loop_t* loop, csilk_io_tcp_t* handle);
int csilk_io_tcp_open(csilk_io_tcp_t* handle, csilk_io_os_sock_t sock);
int csilk_io_tcp_bind(csilk_io_tcp_t* handle, const struct sockaddr* addr, unsigned int flags);
int csilk_io_listen(csilk_io_stream_t* stream, int backlog, csilk_io_connection_cb cb);
int csilk_io_accept(csilk_io_stream_t* server, csilk_io_stream_t* client);
int csilk_io_tcp_nodelay(csilk_io_tcp_t* handle, int enable);
int csilk_io_tcp_keepalive(csilk_io_tcp_t* handle, int enable, unsigned int delay);
int csilk_io_tcp_getpeername(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen);
int csilk_io_tcp_getsockname(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen);
int csilk_io_ip4_addr(const char* ip, int port, struct sockaddr_in* addr);
int csilk_io_ip4_name(const struct sockaddr_in* src, char* dst, size_t size);
int csilk_io_ip6_name(const struct sockaddr_in6* src, char* dst, size_t size);

int csilk_io_read_start(csilk_io_stream_t* stream,
                        csilk_io_alloc_cb  alloc_cb,
                        csilk_io_read_cb   read_cb);
int csilk_io_read_stop(csilk_io_stream_t* stream);

int csilk_io_timer_again(csilk_io_timer_t* handle);

int csilk_io_signal_init(csilk_io_loop_t* loop, csilk_io_signal_t* handle);
int csilk_io_signal_start(csilk_io_signal_t* handle, csilk_io_signal_cb cb, int signum);
int csilk_io_signal_stop(csilk_io_signal_t* handle);

static inline const char*
csilk_io_err_name(int err)
{
    if (err < 0) {
        err = -err;
    }
    if (err == 0) {
        return "OK";
    }
    return strerror(err);
}

#else

#include <uv.h>
/** @brief Event loop type (libuv loop). */
typedef uv_loop_t csilk_io_loop_t;
/** @brief Generic handle type (libuv handle). */
typedef uv_handle_t csilk_io_handle_t;
/** @brief Stream handle type (libuv stream). */
typedef uv_stream_t csilk_io_stream_t;
/** @brief TCP/stream connection handle (libuv tcp handle). */
typedef uv_tcp_t csilk_io_tcp_t;
/** @brief Timer handle (libuv timer handle). */
typedef uv_timer_t csilk_io_timer_t;
/** @brief Async wake-up handle (libuv async handle). */
typedef uv_async_t csilk_io_async_t;
/** @brief Signal handle (libuv signal handle). */
typedef uv_signal_t csilk_io_signal_t;
/** @brief Work (thread-pool job) handle (libuv work handle). */
typedef uv_work_t csilk_io_work_t;
/** @brief Write request type (libuv write request). */
typedef uv_write_t csilk_io_write_t;
/** @brief OS file-descriptor type (libuv os fd). */
typedef uv_os_fd_t csilk_io_os_fd_t;
/** @brief File handle type (libuv file descriptor). */
typedef uv_file csilk_io_file_t;

/** @brief Alias of uv_buf_t. */
#define csilk_io_buf_t uv_buf_t
/** @brief Alias of uv_buf_init. */
#define csilk_io_buf_init uv_buf_init
/** @brief Alias of uv_write. */
#define csilk_io_write uv_write
/** @brief Alias of uv_close. */
#define csilk_io_close uv_close

/** @brief Alias of uv_hrtime. */
#define csilk_io_hrtime uv_hrtime
/** @brief Alias of uv_sleep. */
#define csilk_io_sleep uv_sleep

/** @brief Filesystem-event handle (libuv fs-event handle). */
typedef uv_fs_event_t csilk_io_fs_event_t;
/** @brief Timer callback type (libuv timer callback). */
typedef uv_timer_cb csilk_io_timer_cb;
/** @brief Filesystem-event callback type (libuv fs-event callback). */
typedef uv_fs_event_cb csilk_io_fs_event_cb;

/** @brief Loop run mode (libuv run mode). */
typedef uv_run_mode csilk_io_run_mode;
/** @brief Alias of UV_RUN_DEFAULT. */
#define CSILK_IO_RUN_DEFAULT UV_RUN_DEFAULT
/** @brief Alias of UV_RUN_ONCE. */
#define CSILK_IO_RUN_ONCE UV_RUN_ONCE
/** @brief Alias of UV_RUN_NOWAIT. */
#define CSILK_IO_RUN_NOWAIT UV_RUN_NOWAIT

/**
 * @brief Run the event loop until it is stopped or has no active work.
 * @param[in,out] loop Loop to run.
 * @param[in] mode Run mode (default, once, or non-blocking).
 * @return 0 when the loop exits, or a negative error code.
 */
static inline int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
    return uv_run(loop, mode);
}

/**
 * @brief Check whether the event loop has active handles or pending requests.
 */
static inline int
csilk_io_loop_alive(const csilk_io_loop_t* loop)
{
    return uv_loop_alive(loop);
}

/**
 * @brief Initialize a timer handle on a loop.
 * @param[in,out] loop Loop that will own the timer.
 * @param[in,out] handle Timer handle to initialize.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
    return uv_timer_init(loop, handle);
}

/**
 * @brief Start (or restart) a timer.
 * @param[in,out] handle Timer handle.
 * @param[in] cb Callback invoked on each expiration.
 * @param[in] timeout Milliseconds until the first expiration.
 * @param[in] repeat Milliseconds between subsequent expirations (0 = one-shot).
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_timer_start(csilk_io_timer_t* handle,
                     csilk_io_timer_cb cb,
                     uint64_t          timeout,
                     uint64_t          repeat)
{
    return uv_timer_start(handle, cb, timeout, repeat);
}

/**
 * @brief Stop a timer.
 * @param[in,out] handle Timer handle to stop.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_timer_stop(csilk_io_timer_t* handle)
{
    return uv_timer_stop(handle);
}

static inline int
csilk_io_timer_again(csilk_io_timer_t* handle)
{
    return uv_timer_again(handle);
}

static inline int
csilk_io_loop_init(csilk_io_loop_t* loop)
{
    return uv_loop_init(loop);
}

static inline int
csilk_io_loop_close(csilk_io_loop_t* loop)
{
    return uv_loop_close(loop);
}

static inline void
csilk_io_stop(csilk_io_loop_t* loop)
{
    uv_stop(loop);
}

static inline uint64_t
csilk_io_now(const csilk_io_loop_t* loop)
{
    return uv_now(loop);
}

static inline void
csilk_io_update_time(csilk_io_loop_t* loop)
{
    uv_update_time(loop);
}

typedef uv_os_sock_t     csilk_io_os_sock_t;
typedef uv_connection_cb csilk_io_connection_cb;
typedef uv_alloc_cb      csilk_io_alloc_cb;
typedef uv_read_cb       csilk_io_read_cb;
typedef uv_signal_cb     csilk_io_signal_cb;

#define csilk_io_is_active uv_is_active
static inline const char*
_csilk_uv_err_name(int err)
{
    return err == 0 ? "OK" : uv_err_name(err);
}
#define csilk_io_err_name _csilk_uv_err_name

static inline int
csilk_io_tcp_init(csilk_io_loop_t* loop, csilk_io_tcp_t* handle)
{
    return uv_tcp_init(loop, handle);
}

static inline int
csilk_io_tcp_open(csilk_io_tcp_t* handle, csilk_io_os_sock_t sock)
{
    return uv_tcp_open(handle, sock);
}

static inline int
csilk_io_tcp_bind(csilk_io_tcp_t* handle, const struct sockaddr* addr, unsigned int flags)
{
    return uv_tcp_bind(handle, addr, flags);
}

static inline int
csilk_io_listen(csilk_io_stream_t* stream, int backlog, csilk_io_connection_cb cb)
{
    return uv_listen(stream, backlog, cb);
}

static inline int
csilk_io_accept(csilk_io_stream_t* server, csilk_io_stream_t* client)
{
    return uv_accept(server, client);
}

static inline int
csilk_io_tcp_nodelay(csilk_io_tcp_t* handle, int enable)
{
    return uv_tcp_nodelay(handle, enable);
}

static inline int
csilk_io_tcp_keepalive(csilk_io_tcp_t* handle, int enable, unsigned int delay)
{
    return uv_tcp_keepalive(handle, enable, delay);
}

static inline int
csilk_io_tcp_getpeername(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen)
{
    return uv_tcp_getpeername((const uv_tcp_t*)handle, name, namelen);
}

static inline int
csilk_io_tcp_getsockname(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen)
{
    return uv_tcp_getsockname((const uv_tcp_t*)handle, name, namelen);
}

static inline int
csilk_io_ip4_addr(const char* ip, int port, struct sockaddr_in* addr)
{
    return uv_ip4_addr(ip, port, addr);
}

static inline int
csilk_io_ip4_name(const struct sockaddr_in* src, char* dst, size_t size)
{
    return uv_ip4_name(src, dst, size);
}

static inline int
csilk_io_ip6_name(const struct sockaddr_in6* src, char* dst, size_t size)
{
    return uv_ip6_name(src, dst, size);
}

static inline int
csilk_io_read_start(csilk_io_stream_t* stream, csilk_io_alloc_cb alloc_cb, csilk_io_read_cb read_cb)
{
    return uv_read_start(stream, alloc_cb, read_cb);
}

static inline int
csilk_io_read_stop(csilk_io_stream_t* stream)
{
    return uv_read_stop(stream);
}

static inline int
csilk_io_signal_init(csilk_io_loop_t* loop, csilk_io_signal_t* handle)
{
    return uv_signal_init(loop, handle);
}

static inline int
csilk_io_signal_start(csilk_io_signal_t* handle, csilk_io_signal_cb cb, int signum)
{
    return uv_signal_start(handle, cb, signum);
}

static inline int
csilk_io_signal_stop(csilk_io_signal_t* handle)
{
    return uv_signal_stop(handle);
}

/**
 * @brief Initialize a filesystem-event handle on a loop.
 * @param[in,out] loop Loop that will own the handle.
 * @param[in,out] handle Handle to initialize.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_fs_event_init(csilk_io_loop_t* loop, csilk_io_fs_event_t* handle)
{
    return uv_fs_event_init(loop, handle);
}

/**
 * @brief Start watching a path for filesystem events.
 * @param[in,out] handle Handle to start.
 * @param[in] cb Callback invoked on events.
 * @param[in] path Path to watch.
 * @param[in] flags Backend-specific watch flags.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_fs_event_start(csilk_io_fs_event_t* handle,
                        csilk_io_fs_event_cb cb,
                        const char*          path,
                        unsigned int         flags)
{
    return uv_fs_event_start(handle, cb, path, flags);
}

/**
 * @brief Stop watching filesystem events.
 * @param[in,out] handle Handle to stop.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_fs_event_stop(csilk_io_fs_event_t* handle)
{
    return uv_fs_event_stop(handle);
}

#endif

#ifndef CSILK_USE_URING
/**
 * @brief Async wake-up callback type.
 * @param[in,out] handle The async handle that was signalled.
 */
typedef void (*csilk_io_async_cb)(csilk_io_async_t* handle);
/**
 * @brief Initialize an async handle that can wake the loop from another thread.
 * @param[in,out] loop Loop that will own the handle.
 * @param[in,out] async Async handle to initialize.
 * @param[in] async_cb Callback invoked when the handle is signalled.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb)
{
    return uv_async_init(loop, async, async_cb);
}
/**
 * @brief Signal an async handle, waking its loop.
 * @param[in,out] async Async handle to signal.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_async_send(csilk_io_async_t* async)
{
    return uv_async_send(async);
}
#else
/**
 * @brief Initialize an async handle backed by an eventfd.
 * @param[in,out] loop Loop that will own the handle.
 * @param[in,out] async Async handle to initialize.
 * @param[in] async_cb Callback invoked when the handle is signalled.
 * @return 0 on success, or a negative error code.
 */
int csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb);

/**
 * @brief Signal an async handle, waking its loop.
 * @param[in,out] async Async handle to signal.
 * @return 0 on success.
 */
int csilk_io_async_send(csilk_io_async_t* async);
#endif

#ifndef CSILK_USE_URING
/**
 * @brief Work callback type, run on a thread-pool worker.
 * @param[in,out] req The work request being executed.
 */
typedef void (*csilk_io_work_cb)(csilk_io_work_t* req);
/**
 * @brief Completion callback type, run on the loop thread after work finishes.
 * @param[in,out] req The work request that completed.
 * @param[in] status 0 on success, or a negative error code.
 */
typedef void (*csilk_io_after_work_cb)(csilk_io_work_t* req, int status);
/**
 * @brief Queue a function to run on the libuv thread pool.
 * @param[in,out] loop Loop that runs the completion callback.
 * @param[in,out] req Work request (owned by caller until after-work callback).
 * @param[in] work_cb Function executed on a worker thread.
 * @param[in] after_work_cb Function executed on the loop thread afterwards.
 * @return 0 on success, or a libuv error code.
 */
static inline int
csilk_io_queue_work(csilk_io_loop_t*       loop,
                    csilk_io_work_t*       req,
                    csilk_io_work_cb       work_cb,
                    csilk_io_after_work_cb after_work_cb)
{
    return uv_queue_work(loop, req, work_cb, after_work_cb);
}
#else
/**
 * @brief Work callback type, run on a thread-pool worker.
 * @param[in,out] req The work request being executed.
 */
typedef void (*csilk_io_work_cb)(csilk_io_work_t* req);
/**
 * @brief Completion callback type, run after work finishes.
 * @param[in,out] req The work request that completed.
 * @param[in] status 0 on success, or a negative error code.
 */
typedef void (*csilk_io_after_work_cb)(csilk_io_work_t* req, int status);

/**
 * @brief Internal uring worker-queue entry point.
 * @param[in,out] req Work request.
 * @param[in] work_cb Function executed on a worker thread.
 * @param[in] after_work_cb Function executed after work finishes.
 * @return 0 on success, or a negative error code.
 */
extern int _csilk_uring_queue_work(csilk_io_work_t*       req,
                                   csilk_io_work_cb       work_cb,
                                   csilk_io_after_work_cb after_work_cb);

/**
 * @brief Queue a function to run on the uring worker pool.
 * @param[in,out] loop Loop that runs the completion callback (unused).
 * @param[in,out] req Work request (owned by caller until after-work callback).
 * @param[in] work_cb Function executed on a worker thread.
 * @param[in] after_work_cb Function executed after work finishes.
 * @return 0 on success, or a negative error code.
 */
static inline int
csilk_io_queue_work(csilk_io_loop_t*       loop,
                    csilk_io_work_t*       req,
                    csilk_io_work_cb       work_cb,
                    csilk_io_after_work_cb after_work_cb)
{
    (void)loop;
    return _csilk_uring_queue_work(req, work_cb, after_work_cb);
}
#endif

#ifndef CSILK_USE_URING
/** @brief Filesystem request type (libuv fs request). */
typedef uv_fs_t csilk_io_fs_t;

/** @brief File handle type (libuv file descriptor). */
typedef uv_file csilk_io_file_t;

/** @brief Alias of uv_close. */
#define csilk_io_close uv_close
/** @brief Generic handle type (libuv handle). */
typedef uv_handle_t csilk_io_handle_t;
/** @brief Close callback type (libuv close callback). */
typedef uv_close_cb csilk_io_close_cb;

/** @brief Alias of uv_resident_set_memory. */
#define csilk_io_resident_set_memory uv_resident_set_memory
/** @brief Resource-usage snapshot type (libuv rusage). */
typedef uv_rusage_t csilk_io_rusage_t;
/** @brief Alias of uv_getrusage. */
#define csilk_io_getrusage uv_getrusage
/** @brief Alias of uv_strerror. */
static inline const char*
_csilk_uv_strerror(int err)
{
    return err == 0 ? "Success" : uv_strerror(err);
}
#define csilk_io_strerror _csilk_uv_strerror

/** @brief Alias of uv_is_closing. */
#define csilk_io_is_closing uv_is_closing
/** @brief Alias of uv_fileno. */
#define csilk_io_fileno uv_fileno
/** @brief OS file-descriptor type (libuv os fd). */
typedef uv_os_fd_t csilk_io_os_fd_t;

/** @brief Alias of uv_write. */
#define csilk_io_write uv_write
/** @brief Write request type (libuv write request). */
typedef uv_write_t csilk_io_write_t;
/** @brief Stream handle type (libuv stream). */
typedef uv_stream_t csilk_io_stream_t;
/** @brief Write completion callback type (libuv write callback). */
typedef uv_write_cb csilk_io_write_cb;

/** @brief Alias of uv_buf_t. */
#define csilk_io_buf_t uv_buf_t
/** @brief Alias of uv_buf_init. */
#define csilk_io_buf_init uv_buf_init

/** @brief File stat structure type (libuv stat). */
#define csilk_io_stat_t uv_stat_t
/** @brief Alias of uv_fs_fstat. */
#define csilk_io_fs_fstat uv_fs_fstat

/** @brief Alias of uv_fs_open. */
#define csilk_io_fs_open uv_fs_open
/** @brief Alias of uv_fs_close. */
#define csilk_io_fs_close uv_fs_close
/** @brief Alias of uv_fs_read. */
#define csilk_io_fs_read uv_fs_read
/** @brief Alias of uv_fs_write. */
#define csilk_io_fs_write uv_fs_write
/** @brief Alias of uv_fs_fsync. */
#define csilk_io_fs_fsync uv_fs_fsync
/** @brief Alias of uv_fs_sendfile. */
#define csilk_io_fs_sendfile uv_fs_sendfile
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
/** @brief Filesystem request carrying a result, stat, and user data. */
typedef struct {
    ssize_t     result;  /**< Result/error of the last fs operation. */
    struct stat statbuf; /**< Stat buffer populated by fstat. */
    void*       data;    /**< Opaque user data. */
} csilk_io_fs_t;
/**
 * @brief Open a file synchronously via the kernel (used by the uring backend).
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request to populate with the result.
 * @param[in] path Path to open.
 * @param[in] flags POSIX open flags.
 * @param[in] mode Permission bits for newly created files.
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return The resulting file descriptor (or negative errno).
 */
static inline int
csilk_io_fs_open(
    csilk_io_loop_t* loop, csilk_io_fs_t* req, const char* path, int flags, int mode, void* cb)
{
    req->result = open(path, flags, mode);
    return req->result;
}
/**
 * @brief Close a file descriptor.
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request to populate with the result.
 * @param[in] fd File descriptor to close.
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return 0 on success, or a negative errno.
 */
static inline int
csilk_io_fs_close(csilk_io_loop_t* loop, csilk_io_fs_t* req, int fd, void* cb)
{
    req->result = close(fd);
    return req->result;
}
/**
 * @brief Read from a file descriptor into @p nbufs buffers at @p offset.
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request to populate with the result.
 * @param[in] fd File descriptor to read from.
 * @param[in] bufs Array of I/O buffers to fill.
 * @param[in] nbufs Number of buffers.
 * @param[in] offset Byte offset (negative = current position via preadv semantics).
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return Number of bytes read, or a negative errno.
 */
static inline int
csilk_io_fs_read(csilk_io_loop_t*     loop,
                 csilk_io_fs_t*       req,
                 int                  fd,
                 const csilk_io_buf_t bufs[],
                 unsigned int         nbufs,
                 int64_t              offset,
                 void*                cb)
{
    req->result = preadv(fd, (const struct iovec*)bufs, nbufs, offset);
    return req->result;
}
/**
 * @brief Write @p nbufs buffers to a file descriptor at @p offset.
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request to populate with the result.
 * @param[in] fd File descriptor to write to.
 * @param[in] bufs Array of I/O buffers to write.
 * @param[in] nbufs Number of buffers.
 * @param[in] offset Byte offset (-1 = current position).
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return Number of bytes written, or a negative errno.
 */
static inline int
csilk_io_fs_write(csilk_io_loop_t*     loop,
                  csilk_io_fs_t*       req,
                  int                  fd,
                  const csilk_io_buf_t bufs[],
                  unsigned int         nbufs,
                  int64_t              offset,
                  void*                cb)
{
    if (offset == -1) {
        req->result = writev(fd, (const struct iovec*)bufs, nbufs);
    } else {
        req->result = pwritev(fd, (const struct iovec*)bufs, nbufs, offset);
    }
    return req->result;
}
/**
 * @brief Flush a file descriptor's data to stable storage.
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request to populate with the result.
 * @param[in] fd File descriptor to sync.
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return 0 on success, or a negative errno.
 */
static inline int
csilk_io_fs_fsync(csilk_io_loop_t* loop, csilk_io_fs_t* req, int fd, void* cb)
{
    req->result = fsync(fd);
    return req->result;
}

#include <sys/stat.h>
/** @brief File stat structure type (struct stat). */
typedef struct stat csilk_io_stat_t;

/**
 * @brief Populate @p req->statbuf with the stat of @p fd.
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request whose statbuf is filled.
 * @param[in] fd File descriptor to stat.
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return 0 on success, or a negative errno.
 */
static inline int
csilk_io_fs_fstat(csilk_io_loop_t* loop, csilk_io_fs_t* req, int fd, void* cb)
{
    req->result = fstat(fd, &req->statbuf);
    return req->result;
}

/**
 * @brief Copy data from @p in_fd to @p out_fd using sendfile.
 * @param[in,out] loop Loop that owns the request (unused).
 * @param[in,out] req Request to populate with the result.
 * @param[in] out_fd Destination file descriptor.
 * @param[in] in_fd Source file descriptor.
 * @param[in] in_offset Offset into the source (negative = current position).
 * @param[in] length Number of bytes to transfer.
 * @param[in] cb Unused callback parameter (kept for API compatibility).
 * @return Number of bytes transferred, or a negative errno.
 */
int csilk_io_fs_sendfile(csilk_io_loop_t* loop,
                         csilk_io_fs_t*   req,
                         int              out_fd,
                         int              in_fd,
                         int64_t          in_offset,
                         size_t           length,
                         void*            cb);

#endif

#ifndef CSILK_USE_URING
/** @brief Alias of uv_fs_req_cleanup. */
#define csilk_io_fs_req_cleanup uv_fs_req_cleanup
#else
/**
 * @brief Release any resources held by a filesystem request.
 * @param[in,out] req Request to clean up (no-op under io_uring).
 */
static inline void
csilk_io_fs_req_cleanup(csilk_io_fs_t* req)
{
}
#endif

#ifndef CSILK_USE_URING
/** @brief Alias of uv_default_loop. */
#define csilk_io_default_loop uv_default_loop
/** @brief Alias of uv_resident_set_memory. */
#define csilk_io_resident_set_memory uv_resident_set_memory
/** @brief Resource-usage snapshot type (libuv rusage). */
typedef uv_rusage_t csilk_io_rusage_t;
/** @brief Alias of uv_getrusage. */
#define csilk_io_getrusage uv_getrusage
#else
/**
 * @brief Return the process-wide default loop.
 * @return Pointer to the default loop instance.
 */
csilk_io_loop_t* csilk_io_default_loop(void);
#include <sys/resource.h>
/** @brief Resource-usage snapshot type (struct rusage). */
typedef struct rusage csilk_io_rusage_t;
/**
 * @brief Query the resident set size of the current process.
 * @param[out] rss Receives the RSS in bytes (set to 0 under io_uring).
 * @return 0 on success (always succeeds under io_uring).
 */
static inline int
csilk_io_resident_set_memory(size_t* rss)
{
    if (!rss) {
        return -1;
    }
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) {
        *rss = 0;
        return 0;
    }
    long pages = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &pages, &resident) == 2) {
        long page_size = sysconf(_SC_PAGESIZE);
        *rss = (size_t)(resident * (page_size > 0 ? page_size : 4096));
    } else {
        *rss = 0;
    }
    fclose(f);
    return 0;
}
/**
 * @brief Retrieve resource usage for the current process.
 * @param[out] rusage Receives the rusage snapshot.
 * @return 0 on success, or a negative errno.
 */
static inline int
csilk_io_getrusage(csilk_io_rusage_t* rusage)
{
    return getrusage(RUSAGE_SELF, rusage);
}
#endif
