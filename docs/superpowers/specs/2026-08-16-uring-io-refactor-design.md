# uring_io.c Module Refactoring Design

**Date**: 2026-08-16  
**Status**: Draft

## Overview

Split `src/core/uring/uring_io.c` (932 lines) into 9 focused modules.

## Target Structure

```
src/core/uring/
├── uring_io.c            # Thin wrapper (include all sub-modules)
├── uring_loop.c          # Loop lifecycle (init/close/stop/now/default_loop)
├── uring_handle.c        # Handle init (async/signal/timer init)
├── uring_tcp.c           # TCP ops (open/bind/listen/accept/nodelay/keepalive/ip helpers)
├── uring_stream.c        # Read ops (read_start/read_stop)
├── uring_write.c         # Write ops (write + write_done callback)
├── uring_close.c         # Close (csilk_io_close)
├── uring_timer.c         # Timer ops (start/stop/again)
└── uring_run.c           # Event loop dispatch (csilk_io_run)
```

## File Responsibilities

### uring_loop.c (~70 lines)
- `csilk_io_loop_init`, `csilk_io_loop_close`
- `csilk_io_stop`, `csilk_io_now`, `csilk_io_update_time`
- `csilk_io_default_loop`

### uring_handle.c (~100 lines)
- `csilk_io_async_init`, `csilk_io_async_send`
- `csilk_io_signal_init`, `csilk_io_signal_start`, `csilk_io_signal_stop`
- `csilk_io_timer_init`

### uring_tcp.c (~150 lines)
- `csilk_io_tcp_init`, `csilk_io_tcp_open`
- `csilk_io_tcp_bind`, `csilk_io_listen`, `csilk_io_accept`
- `csilk_io_tcp_nodelay`, `csilk_io_tcp_keepalive`, `csilk_io_tcp_getpeername`
- `csilk_io_ip4_addr`, `csilk_io_ip4_name`, `csilk_io_ip6_name`

### uring_stream.c (~50 lines)
- `csilk_io_read_start`, `csilk_io_read_stop`

### uring_write.c (~60 lines)
- `csilk_io_write`
- `csilk_uv_on_write_done`

### uring_close.c (~40 lines)
- `csilk_io_close`

### uring_timer.c (~50 lines)
- `csilk_io_timer_start`, `csilk_io_timer_stop`, `csilk_io_timer_again`

### uring_run.c (~250 lines)
- `csilk_io_run` — the full CQE dispatch loop with all opcode handlers

## Shared Dependencies
- `uring_internal.h` — opcode enum, encode/decode helpers (used by all modules)
- `sys_io.h` — public io API

## Non-Goals
- No functional changes
- No API changes
- No new exports
