# ABI Stability Roadmap — Context, Router & Core Opaque Encapsulation

> Status: **Complete** | Completed: v0.5.1 | Based on: docs/meta/ABI_REPORT.md
>
> **ABI Rule**: All public API functions **MUST** take/return opaque handles (`csilk_ctx_t*`, `csilk_router_t*`, `csilk_server_t*`, `csilk_app_t*`, `csilk_mq_t*`) — direct struct field access **MUST NOT** be exposed in public headers. Accessor function call overhead **SHOULD** be zero when inlined (single pointer dereference, ≤ 1 CPU cycle).

## Current State

All internal structure layouts (`csilk_ctx_s`, `csilk_server_s`, `csilk_router_s`, `csilk_app_s`, `csilk_mq_s`, `csilk_raft_s`, `csilk_wf_s`, `csilk_mcp_server_s`, `csilk_db_pool_s`) are strictly confined to `src/**_internal.h`. All public API operates on forward-declared opaque pointer handles.

---

## Completed Phases

### Phase A: Accessor API Expansion ✅
Implemented comprehensive accessor/mutator API in `include/csilk/core/context.h`, covering request, response, routing parameters, key-value storage, and streaming flags.

### Phase B: Middleware & Framework Migration ✅
- Updated all internal modules (`src/`) to use accessor APIs.
- Migrated all built-in middleware modules to pure accessor interactions.

### Phase C: Test & Example Migration ✅
- Updated all test files and example applications to use opaque handles and public APIs.
- Zero internal struct header includes in test or example directories.

### Phase D: Router & Server Opaque Encapsulation ✅
- Converted `csilk_router_t` and `csilk_server_t` into fully opaque handles.
- Moved `struct csilk_router_s` and `struct csilk_server_s` into internal implementation units.

### Phase E: 3rd-Party & Backend Decoupling ✅
- Decoupled OpenSSL from public headers: `include/csilk/core/hash.h` uses 64-bit aligned opaque memory buffers (`csilk_sha1_ctx`, `csilk_sha256_ctx`).
- Decoupled Backend I/O headers from `context.h`: `sys_io.h` and `csilk_get_work_req` relocated to internal headers.
- Unified 6-tier memory ownership model (`BORROWED`, `ARENA`, `OWNED`, `TRANSFER`, `POOL`, `TLS_CACHE`).
