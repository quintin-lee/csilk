# csilk v1.1.0 Roadmap

> **Baseline**: `1.0.0 LTS` | **Planned**: `2026-11-15`
> [中文](../zh-CN/meta/ROADMAP-v1.1.md) | [English](ROADMAP-v1.1.md)

---

## Positioning

**Theme**: Production-grade realisation of experimental stubs + coverage hardening + concurrency fixes.

v1.0.0 LTS explicitly excluded stub modules (WASM, AF_XDP, DPDK) from its stability
commitment. v1.1 delivers the **real implementation path** for the two highest-value
modules (WASM via WAMR, AF_XDP via kernel bypass), fixes known concurrency defects,
and raises whole-tree line coverage to **80%+**, laying the architectural groundwork
for HTTP/3 in v1.2.

---

## v1.0.0 Known Defects (Verified)

| ID | Module | Description | Sev |
|----|--------|-------------|-----|
| L1 | `wasm_plugin.c` | `fuel_limit` decrements cumulatively; after enough calls the plugin is permanently disabled | P1 |
| L2 | `wasm_vm.c` | `csilk_wasm_plugin_exec` returns a simulated JSON string — no real bytecode execution | P0 |
| L3 | `session.c` | Global singly-linked list O(n); no background expiry sweeper | P2 |
| L4 | `session.c` | Process-local store; not shared across nodes | P2 (v1.2) |
| L5 | `otlp_trace.c` | Fixed 2048-span ring buffer silently drops traces under high QPS | P2 |
| L6 | `wf_cluster_sm.c` | Hard-coded `s_bindings[32]`; no way to register >32 workflow↔Raft bindings | P2 |
| L7 | `af_xdp.c` | Simulated RX/TX cursor; `io_perf_probe.c` only checks `geteuid()==0` | P0 |
| L8 | `CSILK_ENABLE_EXPERIMENTAL_STUBS` | Defaults `ON`, contradicting the 1.0.0 stability commitment; must default `OFF` | P1 |
| L9 | Coverage | 69% line coverage (12,609/18,281); `otlp_trace`, `wf_cluster_sm`, `vector_hnsw` persistence paths uncovered | P1 |

---

## Phase 1 — WASM Real Runtime via WAMR (8-10 weeks)

**Goal**: Replace the simulated execution in `wasm_vm.c` with a real WebAssembly
bytecode interpreter (WAMR), so `csilk_wasm_plugin_execute` runs actual `.wasm` code.

### 1.1 Dependency

- **FetchContent**: [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime),
  interpreter mode (`-DWAMR_BUILD_INTERP=1`), no JIT.
- **CMake option**: `-DCSILK_ENABLE_WAMR=ON` (default `OFF`).
  When `ON`, `CSILK_ENABLE_EXPERIMENTAL_STUBS` is forced `OFF`.

### 1.2 API Changes

Extend `struct csilk_wasm_plugin_s` with:
```c
void*       wasm_exec_env;   /* WASM exec env handle */
uint32_t    wasm_module_id;  /* WAMR module descriptor */
bool        use_wamr;        /* true when WAMR backend is active */
```

`csilk_wasm_plugin_exec` implementation path:
1. Load `bytecode` into a WAMR module.
2. Register host imports (`csilk_wasm_host_*` functions) via WAMR import resolution.
3. Map `config.fuel_limit` to WAMR instruction limit.
4. **Fix L1**: reset `fuel_limit` to the initial configured value at the start of every
   call; fuel exhaustion no longer persists across invocations.

### 1.3 Hot Reload Integration

- New API: `csilk_wasm_plugin_hot_reload(plugin, new_bytes, new_size)`.
- Old module retired via EBR grace period; new module activated atomically.
- `hot_reload.c` gains a `wasm_plugin` dispatch hook.

### 1.4 Tests

- `tests/core/test_wasm_real.c`:
  - Load `wat2wasm`-compiled sample `.wasm` → execute `on_request` → assert host header mutation.
  - Fuel-trap regression: 10 consecutive calls then reset — verify L1 is fixed.
  - WASI `fd_write` output capture.
- Fuzz target: `fuzz_wasm` (malformed bytecode + WAMR load path).

**Acceptance**:
- `test_wasm_real` passes with `-DCSILK_ENABLE_WAMR=ON`.
- Default build (WAMR disabled) is bit-for-bit identical to 1.0.0 behaviour.

---

## Phase 2 — AF_XDP Kernel Bypass (4-6 weeks)

**Goal**: Replace the simulated RX/TX cursor in `af_xdp.c` with a real
`AF_XDP` socket, and make `io_perf_probe.c` report kernel features instead of
guessing from `geteuid()`.

### 2.1 Platform Constraints

- Linux ≥ 5.3 + XSK-capable driver (`virtio-net`, `ice`, `bnxt_en`, etc.).
- Requires `CAP_NET_RAW` (root or `capsh`).
- CMake option: `-DCSILK_ENABLE_AF_XDP=ON` (default `OFF`).

### 2.2 Implementation

- `csilk_xdp_socket_new`:
  - `socket(AF_XDP)` + `setsockopt(AF_XDP_UMEM_REG)` to register UMEM.
  - Bind `ifname` + `queue_id`; on failure return `ENOTSUP` + log.
  - `fd` field stores the real socket fd (was hardcoded `-1`).
- `csilk_xdp_rx_burst`:
  - Read `XDP_RX_DESC` from RX ring; return UMEM-offset pointer (zero-copy).
  - `rx_head` advanced by kernel; user-space only consumes.
- `io_perf_probe.c`:
  - Replace `geteuid()==0` check with `/sys/class/net/<ifname>/xsk` existence
    + `xsk_socket_get_features()` probe.

### 2.3 Tests

- `tests/core/test_af_xdp.c`:
  - Non-root / no-XSK: fail-closed assertion (`ENOTSUP` return + log).
  - Root + `virtio-net` (CI container): full RX/TX burst coverage.
- CI: new `af_xdp` job (Ubuntu 24.04 root container); local dev works without root (stub degrades).

**Acceptance**:
- `-DCSILK_ENABLE_AF_XDP=ON` root: RX/TX burst paths covered.
- Non-root: clean fail-closed, no crash.

---

## Phase 3 — Coverage Campaign (parallel, 4 weeks)

**Goal**: Whole-tree line coverage from 69% → **80%+**.

| Module | Current | Target | New tests |
|--------|---------|--------|-----------|
| `src/middleware/otlp_trace.c` | ~55% | 90% | Cross-module child-span chaining; `traceparent` inject/propagate |
| `src/middleware/otlp_exporter.c` | ~60% | 85% | Async batch export + backpressure + HTTP 429 retry |
| `src/workflow/wf_cluster_sm.c` | ~70% | 90% | 3-node Raft snapshot restore + command replay idempotency |
| `src/drivers/vector/vector_hnsw.c` | ~80% | 95% | Binary snapshot persist + recall-rate >95% verification |
| `src/middleware/ratelimit.c` (distributed) | ~71% | 85% | Redis storage mock + distributed rate-limit integration (CI container) |
| `src/core/plugin/wasm_plugin.c` | ~80% | 95% | Phase 1 real-WAMR tests |
| `src/core/io/af_xdp.c` (real) | 0% | 80% | Phase 2 new tests |
| `src/messaging/raft_snapshot.c` | ~75% | 90% | Snapshot install + log compaction truncation |

Coverage gate: `.gcovr` target 80%; CI coverage job fails below 78%.

**Acceptance**: `ctest -E test_integration` all green + gcovr report ≥ 80%.

---

## Phase 4 — Concurrency & Correctness Fixes (3 weeks)

### 4.1 `session.c` Hash Sharding (fix L3)

Replace the global linked list with a **sharded hash table** (1024 buckets, per-bucket mutex):
- `csilk_session_get(id)` → hash to bucket → linear scan within bucket (typically 1-2 hops).
- Background `uv_timer` sweeps every 60 s to purge expired sessions.
- `csilk_session_set_max(s, N)`: LRU eviction when over limit.
- Test: `tests/middleware/test_session_concurrent.c` (4 threads × 10K sessions).

### 4.2 `otlp_trace.c` Dynamic Expansion (fix L5)

- Per-worker 1024-span ring buffer (replacing the global fixed 2048 buffer).
- On overflow: configurable sampling rate (default 1/1000) instead of silent drop-all.
- Test: `test_otlp_trace_overflow` (inject 5000 spans, verify sampling degradation, no crash).

### 4.3 `wf_cluster_sm.c` Parametric Binding Limit (fix L6)

- `WF_CLUSTER_MAX_BINDINGS` → CMake option `CSILK_WF_CLUSTER_MAX_BINDINGS` (default 128).
- Overflow returns `-E2BIG`.
- Test: `test_wf_cluster_sm_overflow` (register 129 bindings; 129th returns error).

### 4.4 `CSILK_ENABLE_EXPERIMENTAL_STUBS` Default Flip (fix L8)

- `cmake/options.cmake`: `option(CSILK_ENABLE_EXPERIMENTAL_STUBS ... OFF)` (was `ON`).
- All stub paths already return `ENOTSUP` when `OFF` — no code changes needed.
- Doc sync: README / CHANGELOG / AGENTS.md update to "default fail-closed".

**Acceptance**:
- TSAN full test run (`USE_TSAN=ON`): zero data-race reports.
- `session.c` concurrent test: P99 < 1 ms under 10K sessions (vs O(n) degradation).

---

## Phase 5 — HTTP/3 Architecture Foundation (2 weeks)

**Goal**: No HTTP/3 implementation (deferred to v1.2), but prepare the build
system and internal type layout for ngtcp2/nghttp3 integration.

- `cmake/dependencies.cmake`: optional FetchContent block (`-DCSILK_ENABLE_HTTP3=ON`, default `OFF`):
  ```cmake
  if(CSILK_ENABLE_HTTP3)
    FetchContent_Declare(ngtcp2  GIT_REPOSITORY ... GIT_TAG v1.x)
    FetchContent_Declare(nghttp3 GIT_REPOSITORY ... GIT_TAG v1.x)
  endif()
  ```
- `src/protocols/h3_internal.h` (new): internal `csilk_h3_session_t` definition
  (ngtcp2 conn handle + nghttp3 session), compiled only when `CSILK_ENABLE_HTTP3=ON`.
- `fuzz/fuzz_h3` extension: seed corpus with known-correct frame sequences.
- `tests/protocols/test_h3_handshake.c` (new, conditional):
  - Compiled only when `CSILK_ENABLE_HTTP3=ON` AND system OpenSSL ≥ 3.2.

**Acceptance**:
- `-DCSILK_ENABLE_HTTP3=ON` compiles cleanly.
- Default build (OFF): zero regression; `libcsilk.so` symbol table unchanged.

---

## Task Breakdown

| ID | Phase | Content | Est. (days) | Deps |
|----|-------|---------|:-----------:|------|
| T1.1 | WASM | WAMR FetchContent + CMake option | 3 | — |
| T1.2 | WASM | `wasm_plugin_t` extension + WAMR load path | 5 | T1.1 |
| T1.3 | WASM | WASI host-imports implementation (replace constant stubs) | 4 | T1.2 |
| T1.4 | WASM | Fuel limit fix (L1) + hot-reload API | 3 | T1.2 |
| T1.5 | WASM | `test_wasm_real` + `fuzz_wasm` + CI | 4 | T1.3 |
| T2.1 | AF_XDP | Real socket/UMEM implementation | 5 | — |
| T2.2 | AF_XDP | `io_perf_probe.c` kernel feature probing | 2 | T2.1 |
| T2.3 | AF_XDP | CI root-container job + local fail-closed tests | 3 | T2.1 |
| T3.1 | Cov. | `otlp_trace`/`otlp_exporter` integration tests | 5 | — |
| T3.2 | Cov. | `wf_cluster_sm` snapshot-restore tests | 4 | — |
| T3.3 | Cov. | `vector_hnsw` persist + recall-rate tests | 4 | — |
| T3.4 | Cov. | `ratelimit` distributed Redis mock tests | 3 | — |
| T3.5 | Cov. | gcovr gate 80% + CI config | 2 | T3.1–3.4 |
| T4.1 | Conc. | `session.c` sharded hash + expiry timer | 5 | — |
| T4.2 | Conc. | `otlp_trace.c` dynamic per-worker buffer + sampling | 3 | — |
| T4.3 | Conc. | `wf_cluster_sm.c` parametric binding limit | 1 | — |
| T4.4 | Conc. | `CSILK_ENABLE_EXPERIMENTAL_STUBS` default flip | 1 | T4.1–4.3 |
| T5.1 | H3 | ngtcp2/nghttp3 FetchContent + internal type defs | 3 | — |
| T5.2 | H3 | `fuzz_h3` extension + conditional test skeleton | 3 | T5.1 |
| T6 | Docs | ROADMAP / CHANGELOG / AGENTS.md sync | 2 | all |

**Total estimate**: ~72 person-days (~14-16 weeks sequential; **10-12 weeks** with parallel workstreams).

---

## Release Cadence

```
v1.1.0-rc1  (Phase 1 + Phase 4 complete)
  └─ WASM WAMR real runtime + concurrency fixes + fail-closed default

v1.1.0-rc2  (Phase 2 + Phase 3 complete)
  └─ AF_XDP real kernel bypass + coverage ≥ 80%

v1.1.0      (Phase 5 complete + full CI green)
  └─ HTTP/3 architecture groundwork + full release (SOVERSION remains 1)
```

---

## ABI Compatibility

v1.1.0 maintains `SOVERSION = 1`.

**New public APIs** (additive, backward-compatible):
- `csilk_wasm_plugin_hot_reload()`
- `CSILK_WF_CLUSTER_MAX_BINDINGS` compile-time option
- `csilk_h3_session_t` (compiled only when `CSILK_ENABLE_HTTP3=ON`)

**Behavioural changes** (semantics unchanged, defects fixed):
- `csilk_wasm_plugin_exec` fuel reset (L1 fix)
- `CSILK_ENABLE_EXPERIMENTAL_STUBS` default flip to `OFF` (compile-time, not ABI)

**No public symbol removals.**

---

## Out of Scope for v1.1.0 (deferred to v1.2+)

- HTTP/3 full implementation (ngtcp2 + nghttp3 transport layer)
- Native gRPC bidirectional streaming
- Clustered Session sharing (Redis + `csilk_mq`)
- DPDK real implementation or formal removal
- Windows/MSVC port evaluation

---

## Implementation Progress (as of 2026-11-15)

| Phase | Item | Status | Commit |
|-------|------|--------|--------|
| 4.1 | session.c sharded hash table + LRU cap | ✅ Done | `eca05613` |
| 4.2 | otlp_trace.c dynamic buffer + sampling | ✅ Done | `6bd35d50` |
| 4.3 | wf_cluster_sm.c parametric binding limit | ✅ Done | `45fbd1aa` |
| 4.4 | CSILK_ENABLE_EXPERIMENTAL_STUBS default OFF | ✅ Done | `2d6eff55` |
| 1.1 | WAMR 2.4.4 FetchContent + CMake option | ✅ Done | `329ab773` |
| 1.2 | wasm_plugin_t WAMR load path | ✅ Done | `6895a6a6` |
| 1.3 | WASI host-imports | ✅ Done | `6895a6a6` |
| 1.4 | Fuel budget fix + hot-reload API | ✅ Done | `6895a6a6` |
| 1.5 | test_wasm_real + fuzz_wasm + CI | ✅ Done | `5b95ac4e` |
| 2.1 | AF_XDP real socket/UMEM/ring init | ✅ Done | `1269527a` |
| 2.2 | io_perf_probe.c kernel feature detection | ✅ Done | `3c37ffc4` |
| 2.3 | AF_XDP fail-closed test | ✅ Done | `e8e68a05` |
| 3.1 | otlp_trace/otlp_exporter coverage tests | ✅ Done | `09e05a84` |
| 3.2 | wf_cluster_sm snapshot recovery test | ✅ Done | `989583ba` |
| 3.3 | vector_hnsw 1000x128 recall >95% | ✅ Done | `56e2818c` |
| 3.4 | ratelimit distributed Redis mock test | ✅ Done | `c8aed585` |
| 3.5 | gcovr 80% gate + CI config | ✅ Done | `16c429cf` |
| 5.1 | ngtcp2/nghttp3 FetchContent + type defs | ✅ Done | `1170404e` |
| 5.2 | fuzz_h3 + conditional compile test skeleton | ✅ Done | `acf337c7` |
