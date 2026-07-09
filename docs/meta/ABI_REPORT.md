# ABI Stability Assessment Report

> Updated: 2026-05-31 | Assessment of `csilk_ctx_s` opaque type conversion (docs/meta/PLAN.md P2-1)

## Summary

**Status: COMPLETE** — The opaque type conversion has been fully implemented as of v0.3.0.

Internal struct definitions (`csilk_ctx_s`, `csilk_client_t`, `csilk_server_s`) have been
moved from `include/csilk/core/` to `src/core/` (`ctx_types.h`, `srv_impl.h`, `srv_internal.h`).
All non-framework code accesses context state exclusively through the public accessor API.

---

## Current State

### Public API — Opaque forward declaration only
```c
typedef struct csilk_ctx_s csilk_ctx_t;  // include/csilk/types.h — opaque handle
```

### Internal Definitions (hidden from public include/)
```
src/core/ctx_types.h    — csilk_ctx_s layout (30+ fields)
src/core/internal/srv_internal.h    — csilk_server_s layout
src/core/internal/srv_impl.h     — Internal server implementation details
```

### Accessor API (complete coverage)

| Accessor | Provides |
|---|---|
| `csilk_get_method` / `csilk_get_path` / `csilk_get_body` | Request metadata |
| `csilk_get_header` / `csilk_get_query` / `csilk_get_cookie` | Request headers/params |
| `csilk_get_param` / `csilk_get_params_count` | URL path parameters |
| `csilk_get_status` / `csilk_set_header` / `csilk_add_header` | Response control |
| `csilk_get_arena` / `csilk_set` / `csilk_get` | Arena + key-value storage |
| `csilk_is_websocket` / `csilk_is_sse` / `csilk_is_aborted` | Protocol mode flags |
| `csilk_is_async` / `csilk_ctx_set_async` | Async response mode |
| `csilk_get_handler_index` / `csilk_get_work_req` | Handler chain state |
| `csilk_get_file_fd` / `csilk_set_file_response` | Zero-copy file I/O |
| `csilk_get_response_body` / `csilk_set_response_body` | Response body manipulation |
| `csilk_ctx_get_server` / `csilk_ctx_get_mq` | Framework object access |
| `csilk_ctx_defer` / `csilk_ctx_defer_free` | Panic-safe resource cleanup |
| `csilk_for_each_header` / `csilk_for_each_query` / `csilk_for_each_form_field` | Iteration API |
| `csilk_bind_json` / `csilk_bind_reflect` / `csilk_parse_form_urlencoded` | Body parsing |

### Public headers that use opaque types only
```
include/csilk/types.h       — forward declaration only
include/csilk/context.h     — all accessors, no struct dereference
include/csilk/response.h    — status/header/body/redirect/streaming API
include/csilk/router.h      — route registration and matching
include/csilk/server.h      — server lifecycle
include/csilk/middleware.h  — 15 built-in middleware
include/csilk/hooks.h       — lifecycle hooks
include/csilk/websocket.h   — WebSocket API
include/csilk/sse.h         — SSE API
include/csilk/mq.h          — Message Queue API
include/csilk/workflow.h    — Workflow engine API
include/csilk/admin.h       — Admin dashboard API
include/csilk/group.h       — Route groups
include/csilk/config.h      — Configuration types
include/csilk/errors.h      — Error codes
include/csilk/version.h     — Version info (generated from version.h.in)
```

### Migration completed for:
- [x] 15 built-in middleware — all use accessor API exclusively
- [x] 30+ test files — removed `#include "ctx_types.h"`, use public API
- [x] 11 example programs — moved to public accessor API
- [x] Internal headers physically moved to `src/core/` (commit: 830daca)
- [x] Iteration API (`csilk_for_each_*`) added for header/query/form traversal
- [x] Deferred cleanup API protects against resource leaks across `longjmp`

---

## Future Considerations (v1.0+)

The opaque conversion is functionally complete for v0.3.0. For a future v1.0 ABI freeze:
1. Consider making `csilk_router_t` and `csilk_server_t` similarly opaque.
2. Add a versioned ABI compatibility check in `csilk_server_new`.
3. Formalize the driver vtable layout with reserved padding for future expansion.
