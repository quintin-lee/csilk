# csilk v1.2.0 Roadmap

## Overview

v1.2.0 focuses on **protocol completeness** (HTTP/3, gRPC), **session abstraction** (pluggable backends), and **production hardening** (security audit, memory optimization).

## Phases

### Phase 1: HTTP/3 (QUIC) — ✅ Complete

| Task | Status |
|---|---|
| 1.1 ngtcp2 QUIC transport | ✅ `src/protocols/http3.c` |
| 1.2 nghttp3 session (QPACK) | ✅ `src/protocols/h3_session.c` |
| 1.3 TLS 1.3 + QUIC (`ngtcp2_crypto_ossl`) | ✅ `src/protocols/http3.c` (OpenSSL ≥ 3.2) |
| 1.4 Shared router/middleware | ✅ Automatic via `csilk_server_t` / `csilk_app_t` |
| 1.5 Tests + E2E + fuzz + CI | ✅ `tests/protocols/test_h3.c`, `test_h3_worker.c`, `test_h3_e2e.c` |

**Key decisions:**
- System ngtcp2 v1.25.0 / nghttp3 v1.18.0 preferred over FetchContent
- `csilk_h3_state_t` in `src/core/internal/srv_internal.h` (keeps internal state out of `server.h`)
- `CSILK_ENABLE_HTTP3=ON` CMake option (default OFF)

### Phase 2: Session Store — ✅ Complete

| Task | Status |
|---|---|
| 2.1 `csilk_session_store_t` abstraction | ✅ `include/csilk/core/session_store.h` |
| 2.2 Redis backend (hiredis) | ✅ `src/middleware/session_store.c` |
| 2.3 MQ backend (WAL + raft) | ✅ `src/middleware/session_store.c` |
| 2.4 TTL cross-node sync + tests | ✅ `tests/middleware/test_session_store.c` |

**Key decisions:**
- Process-local backend uses 256-bucket sharded hash table (FNV-1a)
- Redis backend uses `SETEX` for TTL, `DEL` for deletion
- MQ backend has local table fallback when MQ unavailable

### Phase 3: gRPC — ✅ Complete

| Task | Status |
|---|---|
| 3.1 Frame parsing + trailer | ✅ `src/protocols/grpc.c` |
| 3.2 Bidi streaming | ✅ `csilk_grpc_stream_t` |
| 3.3 Metadata mapping | ✅ `csilk_grpc_metadata_parse` |
| 3.4 Tests | ✅ `tests/protocols/test_grpc.c` |

**Key decisions:**
- `csilk_grpc_frame_encode_ex` (not `_encode`) to avoid collision with `grpc_gateway.c`
- Streaming uses byte-by-byte frame reassembly with dynamic buffer growth
- 4 stream types: unary, client-streaming, server-streaming, bidi-streaming

### Phase 4: Production Hardening — ✅ Complete

| Task | Status |
|---|---|
| 4.1 Simulated stub retirement | ✅ Experimental DPDK/XDP/WASM stubs removed; fail-closed `ENOTSUP` enforced |
| 4.2 Connection pool optimization | ✅ Implemented via lazy arena allocation & idle recycling |
| 4.3 Hot update (3-component atomic) | ✅ Implemented (`csilk_hot_update` in `src/core/server/server_rcu.c`) |
| 4.4 Security audit | ✅ Synced in [SECURITY_ANALYSIS.md](../../../SECURITY_ANALYSIS.md) |

### Phase 6: Strategic Convergence & Bottom-Up Hardening (v1.2.0-GA) — ✅ Complete

| Task | Status |
|---|---|
| 6.1 OpenTelemetry 2.0 AI Trace ↔ HTTP W3C Spans & Context-Aware LLM Inference (`csilk_ai_chat_ctx`) | ✅ `src/middleware/otlp_trace.c`, `src/drivers/ai/ai.c`, `include/csilk/drivers/ai.h` |
| 6.2 Multi-Agent Work-Stealing Grid ↔ Workflow Integration (`csilk_wf_set_grid`, `csilk_wf_submit_grid_task`) | ✅ `src/workflow/wf_grid.c`, `include/csilk/app/workflow.h` |
| 6.3 WebAssembly (WAMR) Fuel Metering, App Middleware (`csilk_app_use_wasm`) & Workflow Node (`csilk_wf_add_wasm_plugin_node`) | ✅ `src/core/plugin/wasm_plugin.c`, `include/csilk/app/app.h`, `src/workflow/wf_tools.c` |
| 6.4 Eliminate Non-Threadsafe `strtok` & Implement SSRF Protection Engine (`csilk_security_*`) | ✅ `src/core/primitives/ssrf.c`, `include/csilk/core/security.h` |
| 6.5 Triple-Component Atomic Hot Update Engine (`csilk_hot_update`) | ✅ `src/core/server/server_rcu.c`, `include/csilk/core/server/hot_reload.h` |
| 6.6 Keep-Alive Idle Connection Lazy Arena Allocation & On-Demand Recycling | ✅ `src/core/server/connection_io.c`, `src/core/http/http1_pipeline.c` |
| 6.7 Full ABI SOVERSION 2 Compatibility Gate (876 pure exports, 100% `csilk_*`) | ✅ `scripts/check_abi_compatibility.sh` |
| 6.8 Full Test Regression Suite | ✅ `ctest` (Core-Only & Full) |

### Phase 7: Windows — ⚠️ Out of Scope

| Task | Status |
|---|---|
| 7.1 Windows dependency matrix | ✅ Assessed (IOCP, NDIS/WFP, `pthread_barrier_t` gaps; out of scope) |
| 7.2 CMake cross-compile + CI | ❌ Not supported |
| 7.3 Test adaptation | ❌ Not supported |

**Decision**: Windows requires io_uring equivalent (IOCP), AF_XDP equivalent (NDIS/WFP), and pthread_barrier shim. Estimated 8 weeks. Not in v1.2.0 scope.

## Remaining Items (v1.3 candidates)

- [x] 1.3 TLS 1.3 + QUIC (`ngtcp2_crypto_ossl`, OpenSSL ≥ 3.2) — delivered in v1.2.0
- [ ] 4.2 SSL context sharing (TLS 1.3 Session Ticket reuse)
- [x] 4.2 P0: Lazy arena allocation and idle recycling (delivered in v1.2.0-GA)
- [x] 4.3 P0: Three-component atomic hot update implementation (`csilk_hot_update` delivered in v1.2.0-GA)
- [x] Security: `strtok` → `strtok_r` thread safety (delivered in v1.2.0-GA)
- [x] SSRF protection (internal IP and cloud metadata filter delivered in v1.2.0-GA)

## Test Coverage

- **Total tests**: 160 Core-Only / 263+ Full (`ctest` 100% passed)
- **New tests in v1.2.0**: test_h3, test_h3_e2e, test_io_equivalence, test_session_store, test_grpc, test_otlp_ai, test_wf_grid, test_wasm_fuel, test_wf_grid_integration, test_app_wasm, test_ai_ctx_trace, test_ssrf, test_hot_update_atomic, test_lazy_arena
- **Fuzz targets**: 11 harnesses (6 Core + 5 Full: `fuzz_raft_rpc`, `fuzz_mcp`, `fuzz_sql_rewrite`, `fuzz_wasm`, `fuzz_h3`)

## File Inventory

```
src/protocols/http3.c                 # QUIC transport (ngtcp2 v1.25.0)
src/protocols/h3_session.c            # HTTP/3 session (nghttp3 v1.18.0 + QPACK)
src/protocols/grpc.c                  # gRPC frames, trailers, streaming
src/middleware/session_store.c        # Session store (local/Redis/MQ)
include/csilk/protocols/h3.h          # HTTP/3 public API
include/csilk/protocols/grpc.h        # gRPC public API
include/csilk/core/session_store.h    # Session store vtable
tests/protocols/test_h3.c             # HTTP/3 unit/stream tests
tests/protocols/test_h3_e2e.c         # HTTP/3 UDP/QUIC E2E tests
tests/protocols/test_grpc.c           # gRPC tests
tests/middleware/test_session_store.c # Session store tests
tests/core/test_io_equivalence.c      # libuv vs io_uring equivalence tests
```
