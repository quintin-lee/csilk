# AGENTS.md — csilk

## Build & Test

```bash
# Configure (default: Release, clang)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang

# Build everything
cmake --build build -j$(nproc)

# Run unit tests (excludes integration, with timeout)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure

# Run a single test
ctest --test-dir build -R test_bcrypt --timeout 10 --output-on-failure

# Run integration tests separately
ctest --test-dir build -R test_integration --timeout 10 --output-on-failure

# Build and run with io_uring backend (Linux only)
cmake -B build_uring -S . -DCMAKE_BUILD_TYPE=Debug -DCSILK_USE_URING=ON
cmake --build build_uring -j$(nproc)
ctest --test-dir build_uring --timeout 10 --output-on-failure
```

**CI matrix**: `ubuntu-24.04` + `macos-14`, `Debug` + `Release`. ASAN on Linux Debug, TSAN separate job, coverage uses **gcc** (not clang) with `-DENABLE_OOM_TEST=OFF`.

## Sanitizer Builds

```bash
# ASAN (address + leak)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_ASAN=ON -DENABLE_OOM_TEST=ON -DCSILK_BUILD_SHARED=ON

# TSAN (mutually exclusive with ASAN)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_TSAN=ON -DENABLE_OOM_TEST=ON

# Coverage (requires gcc, no TEST_OOM)
cmake -B build -S . -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_COVERAGE=ON -DENABLE_OOM_TEST=OFF
```

## Lint & Format

```bash
cmake --build build --target check-format   # dry-run, fails on mismatch
cmake --build build --target format         # apply clang-format
cmake --build build --target tidy           # clang-tidy (WarningsAsErrors)
./scripts/check_version_sync.sh             # CMake version ↔ git tag
```

Format targets: `src/*.c`, `src/*.h`, `include/*.h`, `tests/*`, `examples/*`.

## Key Gotchas

### TEST_OOM — deterministic builds
`-DENABLE_OOM_TEST=ON` defines `TEST_OOM`, which forces deterministic salts and memory-allocation faking. Without it, bcrypt and other tests use `/dev/urandom` salts — **tests that assert hash equality must be wrapped in `#ifdef TEST_OOM`**.

### bcrypt constants (fixed Aug 2026)
- `CSILK_BCRYPT_CIPHER_OUT = 24` (not 23) — produces 32 base64 checksum chars
- `CSILK_BCRYPT_HASH_LEN = 62` — `$2a$XX$` (7) + 22 salt + 32 checksum + NUL
- `csilk_bcrypt_verify` reads 32 checksum chars from `hash + 29`

### Stack buffers in crypto
Always `memset` stack buffers before partial `memcpy`:
```c
uint8_t pwd_buf[72];
memset(pwd_buf, 0, sizeof(pwd_buf));  // required when len may be 0
memcpy(pwd_buf, password, len);
```

### Eksblowfish datal/datar initialization
`datal` and `datar` must be zeroed **before** the P-array keying loop (Step 2). Omitting this causes non-deterministic results for empty passwords.

### uv_barrier_t must be heap-allocated
Never declare `uv_barrier_t` on the stack when used across threads. Worker threads hold a pointer to the barrier; the main thread destroys it after `uv_barrier_wait()`. Stack allocation causes use-after-free on multi-worker servers. Always `calloc` and `free` the barrier.

### Clang-format across all src/ include/
Run `cmake --build build --target format` before committing. 500+ files were reformatted in Aug 2026; any new file must match the existing style.

### Clang-tidy false positive (suppressed)
`clang-analyzer-core.uninitialized.Assign` fires on `XL ^= p[i]` in `blowfish_encipher` because the analyzer cannot trace through the array-pointer parameter. Suppressed in `.clang-tidy` — do not add `[[maybe_unused]]` or other workarounds.

### No raw pthread_mutex or uv_* in cross-backend code
All server core code in `src/core/server/`, `src/core/http/`, and elsewhere must use `csilk_io_*`, `csilk_thread_*`, `csilk_barrier_*`, and `csilk_mutex_t`/`csilk_cond_t` from `<csilk/core/sys_io.h>` and `<csilk/core/sync.h>`. Never call raw `uv_*` or `pthread_*` directly in the server core.

### Worker-local active_clients confinement
`wp->active_clients` is strictly single-thread-confined to the owning worker thread. It is never thread-safe. Cross-worker operations (such as broadcast or cross-thread notifications) must use `csilk_dispatch(ctx, cb, arg)` to run callbacks on the owning worker thread.

### Arena allocation semantics
`csilk_arena_alloc()` returns uninitialized memory for zero-overhead allocation. If zero-initialized memory is required, always use `csilk_arena_calloc()`. When worker threads exit, call `csilk_arena_flush_free_list()` to release thread-local cached chunks.

### Context storage RAII with csilk_set_ex
When storing heap-allocated objects (e.g. cJSON nodes or driver handles) in the request context, always use `csilk_set_ex(c, key, ptr, destructor)` so they are freed automatically when the request arena resets.

### Outbound streaming backpressure
`csilk_response_write()`, `csilk_sse_send()`, and `csilk_ws_send()` enforce connection-level outbound watermarks. When they return `0`, the queue has reached the high watermark (`write_high_water_mark`) — pause producer output and resume only after `csilk_on_drain()` fires. Returning `-1` indicates queue overflow or error.

### Hot-Reload mutex & temp copy isolation
`hot_reload_ctx_t` embeds `csilk_mutex_t reload_mutex` to ensure file watcher debounce timers and cross-thread manual `csilk_dev_hot_reload_trigger()` never execute concurrently. Temp library copies must be created using `mkstemp(0600)` without predictable fallbacks, and all resources (`new_router`, `new_handle`, `tmp_path`) must be rolled back on OOM.

### Adaptive io_uring loop initialization & cleanup
Under `CSILK_USE_URING`, `csilk_io_loop_init()` performs adaptive queue sizing (1024 -> 512 -> 256 -> 128 -> 64) with proportional operation pool allocation to avoid `-ENOMEM` under constrained `RLIMIT_MEMLOCK` limits. `csilk_server_free()` must close and free `server->loop` whenever `server->loop_owned` is true.

### Router Writer Serialization (config_mutex)
`csilk_server_set_router_full()` locks `server->config_mutex` around writer updates to guarantee strictly monotonic `global_epoch` advancement and retirement list ordering under concurrent writer contention, while all reader paths (`csilk_server_router_acquire`/`release`) remain 100% wait-free.

### Client Lifetime & Generation Defense
`client_destroy()` is strictly single-thread-confined to the owning worker thread. Non-owner threads must enqueue recycling tasks with generation tags (`_csilk_client_recycle_dispatch_cb`). Recycling tasks must verify `client->generation == gen && client->state == CSILK_CONN_CLOSING` before executing destruction, preventing ABA / UAF on reused connection structs.

### Ordered Teardown & Deferred MQ Destruction
During server shutdown, active clients, timers, and `wp->dispatch_async` queues must be drained before worker threads are joined. `csilk_mq_t` teardown is deferred until after all worker threads are joined (`csilk_thread_join`) to prevent asynchronous callbacks from touching destroyed MQ handles.

### internal.h is a public umbrella header
`include/csilk/core/internal.h` must NOT include messaging/internal headers — doing so leaks MQ internals to any file that includes it. Add explicit `#include "messaging/mq_internal.h"` only in files that directly use `_csilk_mq_new()` / `_csilk_mq_free()`.


## Source Layout

| Directory | Purpose |
|---|---|
| `src/core/` | HTTP server, arena, config, JSON, TLS, ctx, http, primitives |
| `src/core/uring/` | io_uring event loop, connection, server, thread pool |
| `src/crypto/` | base64, sha1, bcrypt, blowfish sboxes, crypto dispatch |
| `src/drivers/` | DB (sqlite, postgres, mysql, redis), AI, cipher, vector |
| `src/middleware/` | auth, cors, csrf, jwt, ratelimit, session, etc. |
| `src/protocols/` | websocket, h2, h3, swagger, mcp |
| `src/messaging/` | MQ core, pubsub, raft WAL/RPC/consensus |
| `src/workflow/` | agent engine, scheduler, DSL |
| `tests/crypto/` | SHA-256, HMAC, Base64, random, URL decode tests |
| `tests/security/` | bcrypt, cipher, UUID, JWT security tests |
| `tests/` | Mirrors `src/` module layout |
| `python/` | CFFI/ctypes bindings |

## Modular Sub-Libraries & Targets

Csilk builds both modular components and composite umbrella libraries in static (`.a`) and dynamic (`.so`) forms:

| Target (Alias) | Static Archive | Shared Library | Description |
|---|---|---|---|
| `csilk::core` / `csilk::core_shared` | `libcsilk-core.a` | `libcsilk-core.so` | Core arena, ctx, router, logger, crypto primitives |
| `csilk::http` / `csilk::http_shared` | `libcsilk-http.a` | `libcsilk-http.so` | HTTP/1 server, app, connection pool, middleware |
| `csilk::tls` / `csilk::tls_shared` | `libcsilk-tls.a` | `libcsilk-tls.so` | TLS 1.3 encryption engine, OpenSSL wrapper |
| `csilk::http2` / `csilk::http2_shared` | `libcsilk-http2.a` | `libcsilk-http2.so` | HTTP/2 (nghttp2), HTTP/3 protocol handler |
| `csilk::db` / `csilk::db_shared` | `libcsilk-db.a` | `libcsilk-db.so` | Database abstraction, SQLite, Vector DB / HNSW |
| `csilk::ai` / `csilk::ai_shared` | `libcsilk-ai.a` | `libcsilk-ai.so` | AI LLM client drivers (OpenAI, Ollama, DeepSeek) |
| `csilk::mq` / `csilk::mq_shared` | `libcsilk-mq.a` | `libcsilk-mq.so` | Message queue, PubSub, WAL, Raft consensus |
| `csilk::workflow` / `csilk::workflow_shared` | `libcsilk-workflow.a` | `libcsilk-workflow.so` | Workflow DAG scheduler, DSL, MCP protocols |
| `csilk::csilk` / `csilk::csilk_shared` | `libcsilk.a` | `libcsilk.so` | Full composite umbrella library |

### Application Linking Example
```cmake
find_package(csilk REQUIRED)
target_link_libraries(my_app PRIVATE csilk::http csilk::tls)
```

## Commit Convention

```
type(scope): 🎯 subject
```

Types: `feat ✨`, `fix 🐛`, `docs 📝`, `style 🎨`, `refactor ♻️`, `test ✅`, `build 📦`, `ci 👷`, `chore 🧹`. Emoji goes **after** the colon, one per commit, subject lowercase, imperative mood.

## Python Bindings

```bash
# Build shared lib first, then run Python tests
cmake --build build --target csilk_shared
python3 python/tests/test_csilk.py
```
