/**
 * @file uring_io.c
 * @brief io_uring driver — thin wrapper for backward compatibility.
 *
 * Implementation split across:
 *   uring_loop.c   - loop lifecycle (init/close/stop/now/default_loop)
 *   uring_handle.c - handle init (async/signal/timer init)
 *   uring_tcp.c    - TCP ops (open/bind/listen/accept/nodelay/keepalive/ip)
 *   uring_stream.c - stream read ops (read_start/read_stop)
 *   uring_write.c  - write ops + write_done callback
 *   uring_close.c  - generic handle close
 *   uring_timer.c  - timer ops (start/stop/again)
 *   uring_run.c    - event loop dispatch (csilk_io_run)
 */

#include <csilk/core/sys_io.h>

#ifdef CSILK_USE_URING

#include "uring_internal.h"

#endif
