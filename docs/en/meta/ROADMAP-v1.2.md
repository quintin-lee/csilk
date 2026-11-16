# csilk v1.2.0 Roadmap

## Overview

v1.2.0 focuses on **protocol completeness** (HTTP/3, gRPC), **session abstraction** (pluggable backends), and **production hardening** (security audit, memory optimization).

## Phases

### Phase 1: HTTP/3 (QUIC) — ✅ Complete

| Task | Status |
|---|---|
| 1.1 ngtcp2 QUIC transport | ✅ `src/protocols/http3.c` |
| 1.2 nghttp3 session (QPACK) | ✅ `src/protocols/h3_session.c` |
| 1.3 TLS 1.3 + QUIC | ⏳ Requires OpenSSL ≥ 3.2 |
| 1.4 Shared router/middleware | ✅ Automatic via csilk_app_t |
| 1.5 Tests + fuzz + CI | ✅ `tests/protocols/test_h3.c` |

**Key decisions:**
- System ngtcp2 0.3.1 / nghttp3 1.18.0 preferred over FetchContent
- `csilk_quic_transport_t` vtable in `server.h` (not in h3.h)
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
| 4.1 DPDK decision | ✅ Keep stub (`docs/en/meta/DPDK-DECISION.md`) |
| 4.2 Connection pool optimization | ✅ Plan documented (`docs/en/meta/CONNECTION-POOL-OPTIMIZATION.md`) |
| 4.3 Hot update (3-component atomic) | ✅ Plan documented (`docs/en/meta/HOT-UPDATE-PLAN.md`) |
| 4.4 Security audit | ✅ `docs/en/meta/SECURITY-AUDIT-v1.2.md` |

### Phase 5: Windows — ⚠️ Out of Scope

| Task | Status |
|---|---|
| 5.1 Windows dependency matrix | ✅ Assessed (`docs/en/meta/WINDOWS-ASSESSMENT.md`) |
| 5.2 CMake cross-compile + CI | ❌ Not supported |
| 5.3 Test adaptation | ❌ Not supported |

**Decision**: Windows requires io_uring equivalent (IOCP), AF_XDP equivalent (NDIS/WFP), and pthread_barrier shim. Estimated 8 weeks. Not in v1.2.0 scope.

## Remaining Items (v1.3 candidates)

- [ ] 1.3 TLS 1.3 + QUIC (requires OpenSSL ≥ 3.2)
- [ ] 4.2 P0: SSL context sharing + lazy arena allocation
- [ ] 4.3 P0: Three-component atomic hot update implementation
- [ ] Security: `strtok` → `strtok_r` in mysql.c/redis.c
- [ ] SSRF protection (internal IP blacklist)

## Test Coverage

- **Total tests**: 255+ (unit + integration)
- **New tests in v1.2.0**: test_h3 (3), test_session_store (7), test_grpc (6)
- **Fuzz targets**: 9 harnesses (including fuzz_h2, fuzz_websocket, fuzz_mcp)

## File Inventory

```
src/protocols/http3.c              # QUIC transport (ngtcp2 0.3.1)
src/protocols/h3_session.c         # HTTP/3 session (nghttp3 + QPACK)
src/protocols/grpc.c              # gRPC frames, trailers, streaming
src/middleware/session_store.c    # Session store (local/Redis/MQ)
include/csilk/protocols/h3.h      # HTTP/3 public API
include/csilk/protocols/grpc.h    # gRPC public API
include/csilk/core/session_store.h # Session store vtable
tests/protocols/test_h3.c        # HTTP/3 tests
tests/protocols/test_grpc.c      # gRPC tests
tests/middleware/test_session_store.c # Session store tests
docs/en/meta/DPDK-DECISION.md    # DPDK assessment
docs/en/meta/CONNECTION-POOL-OPTIMIZATION.md
docs/en/meta/HOT-UPDATE-PLAN.md
docs/en/meta/SECURITY-AUDIT-v1.2.md
docs/en/meta/WINDOWS-ASSESSMENT.md
```
