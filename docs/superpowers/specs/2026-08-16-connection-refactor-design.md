# Connection Module Refactoring Design

**Date**: 2026-08-16  
**Status**: Draft  
**Author**: Agnes (AI Assistant)

## Overview

Refactor `src/core/server/connection.c` (1071 lines) into smaller,职责清晰的模块文件。当前单文件混合了连接池管理、状态机、定时器回调、I/O 回调和连接清理逻辑，导致维护困难。

## Target Structure

```
src/core/server/
├── connection_pool.c    # ~250 lines: pool management
├── connection_state.c   # ~80 lines: state machine
├── connection_timer.c   # ~200 lines: timer callbacks
├── connection_close.c   # ~180 lines: close/destroy logic
├── connection_io.c      # ~300 lines: I/O callbacks
└── connection.c         # ~50 lines: public API wrappers
```

## File Responsibilities

### connection_pool.c
- `pool_get()` / `pool_put()` - client struct pooling
- `reset_hot_state()` - mutable field reset
- `pool_get_arena()` / `pool_put_arena()` - arena pooling
- `_csilk_worker_init_arena_pool()` - pre-allocation
- Read buffer pool (3-tier: 4KB/16KB/64KB)
- `alloc_buffer()` callback

### connection_state.c
- `csilk_conn_state_str()` - state to string
- `csilk_conn_set_state()` - state transition with validation
- `csilk_conn_get_state()` - state query

### connection_timer.c
- `on_timer_close()` - timer close callback
- `on_idle_timeout()` / `on_read_timeout()` / `on_write_timeout()` - timeout handlers
- Timer initialization in `on_new_connection`

### connection_close.c
- `client_destroy()` - final teardown
- `on_close()` - TCP handle close callback
- `client_list_add()` / `client_list_remove()` - active list management
- `_csilk_ctx_async_ref_incr/decr()` - async ref counting
- `_csilk_ctx_loop()` - loop accessor

### connection_io.c
- `on_new_connection()` - accept new TCP connections
- `on_read()` - TCP read callback with TLS/WS/HTTP dispatch
- `reject_connection()` - connection-limited rejection
- `on_rejected_close()` - rejected handle cleanup
- `csilk_get_client_ip()` - IP resolution
- `csilk_client_read_start()` / `csilk_client_read_stop()` - read control

### connection.c
- Public API exports (thin wrappers if needed)
- Include guards and documentation

## Changes Required

### Header Updates
- `src/core/internal/srv_impl.h` - update declarations
- Add new internal headers or keep single `srv_internal.h`

### CMakeLists.txt
- Add new source files to server target

### Tests
- Verify existing tests pass after refactor
- No behavior changes expected

## Non-Goals
- No functional changes
- No API changes
- No performance changes
