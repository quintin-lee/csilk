# wf_scheduler.c Module Refactoring Design

**Date**: 2026-08-16  
**Status**: Draft

## Overview

Split `src/workflow/wf_scheduler.c` (840 lines) into 4 focused modules + thin wrapper.

## Target Structure

```
src/workflow/
├── wf_scheduler.c   # thin wrapper (~15 lines)
├── wf_ctx.c         # context lifecycle management
├── wf_wal.c         # WAL event logging
├── wf_node.c        # node execution (workers, timers, dispatch)
└── wf_run.c         # run orchestration (entry points, TTL)
```

## Module Responsibilities

### wf_ctx.c (~200 lines) — Context lifecycle
- `register_active_ctx`, `unregister_active_ctx`, `find_active_ctx` (static helpers)
- `cleanup_ctx_now`, `on_ttl_timer_close` (static helpers)
- `_wf_cleanup_stale_ctx` (CSILK_INTERNAL, called by wf_resume.c)
- `_wf_find_active_ctx` (CSILK_INTERNAL, called by wf_distributed.c)
- `_wf_cleanup_ctx` (CSILK_INTERNAL, called by wf_resume.c, wf_graph.c)

### wf_wal.c (~40 lines) — WAL event logging
- `_wf_wal_log_event` (CSILK_INTERNAL, called by after_worker_cb in wf_node.c)

### wf_node.c (~330 lines) — Node execution
- `worker_cb` — thread pool callback
- `on_retry_timer`, `on_work_timer_close`, `free_work` — timer work helpers
- `after_worker_cb` — main dispatch (217 lines, largest function)
- `on_node_timeout` — timeout callback
- `execute_node` — node enqueue logic
- `_wf_execute_node` (CSILK_INTERNAL, called by wf_resume.c, wf_distributed.c)

### wf_run.c (~270 lines) — Run orchestration
- `on_workflow_ttl` — TTL expiration callback
- `csilk_wf_run` — public entry point
- `csilk_wf_run_traced` — public entry point with tracing
- `_wf_run_ext_internal` (CSILK_INTERNAL, common implementation)

## Cross-Module Dependencies

| Caller | Callee | Mechanism |
|---|---|---|
| wf_node.c → wf_wal.c | `_wf_wal_log_event` | extern declaration in wf_node.c |
| wf_node.c → wf_ctx.c | `_wf_cleanup_ctx` | declared in workflow_internal.h |
| wf_run.c → wf_node.c | `execute_node` (via `_wf_execute_node`) | declared in workflow_internal.h |
| wf_run.c → wf_wal.c | `_wf_wal_log_event` | extern declaration in wf_run.c |
| All modules | `_wf_broadcast` | declared in workflow_internal.h |

## Non-Goals
- No functional changes
- No API changes
- No new exports
- `node_work_t` stays in `workflow_internal.h`
