# HTTP/3 & QUIC Feasibility Assessment

> Date: 2026-05-30 | Version: 0.5.0-dev

## Summary

HTTP/3 (QUIC) integration is **feasible but high-effort**. Primary blocker is
the lack of a stable, well-maintained QUIC library with a permissive license
that integrates cleanly with libuv's event loop.

## QUIC Library Options

| Library | Language | License | Status | Integration Difficulty |
|---------|:--------:|:-------:|:------:|:----------------------:|
| **lsquic** | C | MIT | Active (LiteSpeed) | Medium — event engine abstraction |
| **msquic** | C | MIT | Active (Microsoft) | High — Windows-first design, POSIX support immature |
| **ngtcp2** | C | MIT | Active (nghttp2 org) | Medium — designed to pair with nghttp3 |
| **quiche** | C/Rust | BSD-2 | Active (Cloudflare) | Medium — Rust core, C FFI |
| **quictls** | C | Apache-2 | Active (OpenSSL fork) | Low — just TLS, needs transport layer |

## Recommendation: ngtcp2 + nghttp3

**Rationale**: csilk already uses nghttp2 for HTTP/2. The ngtcp2 library
(QUIC transport) and nghttp3 (HTTP/3 mapping) are from the same organization
and share coding conventions. Both are MIT-licensed.

**Architecture fit**: ngtcp2 provides a callback-driven API that can be
adapted to libuv's event loop, similar to how nghttp2 already is.

## Integration Plan (v1.0+)

### Step 1: QUIC Transport (ngtcp2)

- Add `ngtcp2` and `ngtcp2_crypto_quictls` as FetchContent dependencies
- Create `src/protocols/quic.c` connected to libuv UDP sockets
- Handle QUIC handshake, connection migration, 0-RTT
- ~2,000 lines estimated

### Step 2: HTTP/3 Mapping (nghttp3)

- Add `nghttp3` as FetchContent dependency
- Layer HTTP/3 on top of QUIC transport
- Map QPACK (header compression) and stream types
- ~1,500 lines estimated

### Step 3: ALPN & Protocol Selection

- Extend `tls.c` ALPN callback to offer `h3` alongside `h2` and `http/1.1`
- Add `Alt-Svc` header support for HTTP/3 upgrade hints
- ~300 lines estimated

### Step 4: Unified Dispatch

- Share the same router, middleware, and handler chain with HTTP/1.1 and HTTP/2
- Protocol transparent to handlers — existing handlers work unchanged

## Estimated Effort

| Phase | Effort | Risk |
|-------|:------:|:----:|
| QUIC transport | 3-4 weeks | Medium — UDP event loop integration |
| HTTP/3 mapping | 2-3 weeks | Low — nghttp3 API is clean |
| ALPN & discovery | 1 week | Low |
| Testing & hardening | 2-3 weeks | Medium |
| **Total** | **8-11 weeks** | |

## Dependencies to Watch

- ngtcp2 requires OpenSSL with QUIC support (OpenSSL 3.2+ or quictls fork)
- QPACK (nghttp3) adds ~200KB to binary size
- QUIC uses UDP — may not work through all firewalls/load balancers

## Verdict

**Defer to v1.1+**. HTTP/3 is still < 30% of global web traffic (2026).
csilk's current HTTP/2 support covers the vast majority of use cases.
Revisit when ngtcp2/nghttp3 reach v1.0 and OpenSSL QUIC support stabilizes.

**Requirements for v1.1+**: QUIC transport **MUST** use ngtcp2 (same organization as nghttp2). HTTP/3 mapping **MUST** use nghttp3. OpenSSL **MUST** be 3.2+ or quictls fork for QUIC crypto. Binary size increase **SHOULD NOT** exceed ~200KB (QPACK). UDP event loop integration **MUST NOT** block the main libuv thread.
