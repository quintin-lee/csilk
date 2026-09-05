# Repository Guidelines — csilk

A lightweight HTTP web framework written in C23, delivering **P99 latency ≤ 5ms under 10K QPS** on commodity hardware. Built on libuv (default) or io_uring (Linux-only opt-in), llhttp, nghttp2, cJSON, and yyjson.

## Project Overview

**Purpose**: Production-grade, embeddable HTTP framework for C services — web server, API gateway, proxy, and embedded service host. Designed for high concurrency (lock-free per-worker connection pool, ~200K QPS linear scaling on 16 cores), low memory footprint (< 2 MB RSS per 10K keep-alive connections), and hot-reloadable routers.

**Language**: C23 (`CMAKE_C_STANDARD 23`). Keywords `static constexpr`, `nullptr`, `bool` are built-in — no `<stdbool.h>`.

**Supported compilers**: GCC 13+ (coverage/Ubuntu target) or Clang 19+ (default CI/fuzzing). **Apple Clang and MSVC are NOT supported** (missing C23 `constexpr` and POSIX APIs).

**Supported OS**: Linux (Ubuntu 24.04 primary CI); macOS (single-worker only — `pthread_barrier_t` unavailable in multi-worker mode on macOS-14). Windows not planned. musl/Alpine untested.
## Tool & Skill Constraints

Use these rules whenever performing work in this repository. They ensure correctness, reproducibility, and alignment with the project's conventions.

### Code discovery

- **Before reading any source file**: run `codegraph_explore` (or `search_graph` / `trace_path`) to locate the symbol, understand its callers/callees, and see the blast radius. One call usually answers the whole question.
- **For literal/text searches** (error messages, string literals, config keys, non-code files): use `grep`, not shell `grep`/`rg`.
- **For file/directory listing**: use `glob`, not `ls`/`find`.
- **For structural edits** (renames, AST-aware rewrites): use `lsp` for references/definition/hover; never manual cross-file renames.

### Editing C source

- **Read first**: always `read` the target file (or the relevant line range) before editing. Stale reads cause rejected edits.
- **Surgical edits**: use the `edit` tool for line-anchored patches. NEVER `write` an entire file to change a few lines.
- **Ranges**: keep ranges tight — changed lines only. Whole construct edits use `PUT N*:`; internal lines use `PUT N.=M:`.
- **Multi-file changes**: name one integration owner; serialize only the irreducibly shared mutation boundary. Do not fan out edits to the same file.
- **Format after editing**: run `cmake --build build --target format` before committing. The tree was normalized in Aug 2026; new files must match.

### Building & testing

- **Configure once per variant**: each sanitizer/backend combination needs its own `-B build_*` directory. Do not re-run configure on an existing build dir — it mutates state.
- **Clean scratch build trees**: after experiments land, run `scripts/clean_builds.sh` (dry-run by default; `--prune` to delete). It keeps the canonical trees — `build`, `build_uring`, `build_asan`, `build_tsan`, `build_fuzz` — and only touches directories containing a `CMakeCache.txt`.
- **Run tests via `ctest`**: never `make test` or invoke test executables directly. Use `--test-dir build` (or the variant dir) and filter with `-R` / `-E`.
- **ASAN/TSAN runtime**: mirror CI flags (`ASAN_OPTIONS`, `LSAN_OPTIONS`, `TSAN_OPTIONS`) when running tests under sanitizers locally. Suppressions live under `cmake/`.
- **Timeout-sensitive tests**: `test_timeout` (10s), `test_multi_worker`/`test_mq_concurrent` (30s), `test_ws_concurrent`/`test_sse_concurrent` (15s). Set `--timeout` explicitly.
- **OOM/deterministic tests**: wrap hash-equality assertions in `#ifdef TEST_OOM`; enable with `-DENABLE_OOM_TEST=ON`.

### Benchmarking & profiling

- **Native CPU tuning**: benchmarks and profiling builds must use `-DCSILK_ENABLE_NATIVE_ARCH=ON` to append `-march=native`.
- **Flame graphs**: `src/util/flamegraph.c` provides backtrace sampling; excluded from coverage by `.gcovr`.
- **Run benchmarks**: `scripts/run_benchmarks.sh`; compare against prior results with `scripts/compare_benchmarks.py`.

### Fuzzing

- **Build fuzzers**: `cmake --build build --target fuzz_test fuzz_yaml fuzz_url fuzz_headers` (requires clang-19 + ASAN).
- **Run**: `./fuzz_test -max_total_time=120 -dict=fuzz/fuzz.dict fuzz/corpus/fuzz_test` (etc.).
- **Add corpus**: seed corpora live in `fuzz/corpus/<name>/`; dictionary at `fuzz/fuzz.dict`.

### Documentation & diagrams

- **Mermaid diagrams**: use Mermaid syntax in markdown code blocks for architecture/flow diagrams. Validate with `python3 scripts/check_mermaid.py .`.
- **Doxygen**: run `cmake --build build --target doxygen` (configured via `docs/Doxyfile.in`).
- **Multi-language docs**: update both `docs/en/` and `docs/zh-CN/` in parallel.

### Version management

- **Single source of truth**: version lives in `cmake/options.cmake` (`CSILK_VERSION_MAJOR/MINOR/PATCH`). NEVER edit version strings in other files directly.
- **Bump**: run `scripts/tag-release.sh <new-version>` — it updates all 18 locations.
- **Verify**: `./scripts/check_version_sync.sh` (or `--expected <ver>`).

### Python bindings

- **Build shared lib first**: `cmake --build build --target csilk_shared`.
- **Run tests**: `python3 python/tests/test_csilk.py`.
- **Package**: `python/setup.py` drives PyPI wheel generation.

### Git workflow

- **Commit convention**: `type(scope): 🎯 subject` with emoji after colon (e.g., `feat(server): 🔥 add RCU router swap`). See full type list in this document.
- **Pre-commit**: run `format` and `check-format` (dry-run) to catch style issues before pushing.

### Prohibited patterns

- **Never** call raw `uv_*` or `pthread_*` directly in `src/core/server/`, `src/core/http/`, or cross-backend code. Use `csilk_io_*`, `csilk_thread_*`, `csilk_barrier_*`, `csilk_mutex_t`/`csilk_cond_t`.
- **Never** declare `uv_barrier_t` on the stack when used across threads — always `calloc`/`free`.
- **Never** use `csilk_arena_alloc()` when zero-initialized memory is required — use `csilk_arena_calloc()`.
- **Never** call `client_destroy()` from a non-owner worker thread — enqueue a recycling task via `csilk_dispatch()`.
- **Never** include `messaging/mq_internal.h` transitively through `include/csilk/core/internal.h` — add explicit includes only where needed.
- **Never** assert bcrypt/hash equality without `#ifdef TEST_OOM` guard (tests use `/dev/urandom` salts by default).

## Architecture & Data Flow

### High-level request lifecycle

```
TCP/SSL accept → worker loop receives event (libuv/io_uring)
  → csilk_client_t allocated from per-worker lock-free pool
  → SSL read into arena-allocated buffer (zero-copy string views)
  → llhttp parser fills csilk_str_view_t for URL/headers/body
  → router trie lookup (AVX2/NEON SIMD accelerated, ~50ns/lookup)
  → middleware chain executes in registration order:
      recovery → logger → auth/JWT → CORS → ratelimit → CSRF → waf → ...
  → handler invoked via csilk_next() callback chain
  → response built in arena; streaming via watermarked queues (backpressure at 64KB high / 16KB low)
  → connection returns to pool (keep-alive) or closed (generation-tagged recycle)
```

### Key structs

| Struct | Location | Role |
|---|---|---|
| `csilk_server_t` | `include/csilk/core/server.h` + opaque impl | Server state: workers, config, router epoch, loops |
| `csilk_ctx_t` | `include/csilk/core/context.h` + opaque impl | Per-request arena, headers, body views, storage, defer chain |
| `csilk_client_t` | `src/core/server/connection.c` internal | Connection lifecycle: state machine (INIT→ACCEPTED→TLS→READING→PROCESSING→WRITING→STREAMING→CLOSING→CLOSED), generation tag, pending I/O counters |
| `csilk_router_t` | `include/csilk/core/router.h` + opaque impl | Segment-prefix trie with RCU/swappable readers |
| `csilk_app_t` | `include/csilk/app.h` | High-level app builder (hooks, groups, middleware registration) |
| `csilk_mq_t` | `include/csilk/messaging/mq.h` | Asynchronous thread-safe message queue with PubSub and WAL |
| `csilk_vector_db_t` | `include/csilk/drivers/db/vector.h` | HNSW skip-graph vector index engine |

### Arena allocation model

- Per-request arena (`csilk_ctx_t` owns it) with ~3 CPU instructions per alloc.
- `csilk_arena_alloc()` — zero-overhead, uninitialized. For zero-init use `csilk_arena_calloc()`.
- Multi-tier chunk free list: 4 KB / 16 KB / 64 KB tiers with `max_total_bytes` cap.
- Reset cost ≤ 5 ns. Free-list flushed on worker exit via `csilk_arena_flush_free_list()`.
- Heap objects bound to arena lifetime via `csilk_set_ex(c, key, ptr, destructor)`.

### Middleware chain

Middleware is a linked function list executed in registration order. Each receives `csilk_ctx_t *c` and calls `csilk_next(c)` to delegate. A middleware that sends a response directly must NOT call `csilk_next()`. The recovery middleware wraps the entire chain in a `csilk_panic()` → deferred-cleanup (LIFO) → 500 path.

### Router & RCU

Router uses a segment-prefix trie. Swaps are serialized under `server->config_mutex` ensuring strictly monotonic `global_epoch` advancement. Readers (`csilk_server_router_acquire` / `release`) are 100% wait-free — no lock held during lookup. EBR grace periods ensure concurrent readers finish before retired trie nodes are freed.

### Concurrency model

- **Worker threads** (one per CPU core by default) each own a libuv/io_uring event loop.
- `wp->active_clients` is strictly single-thread-confined to the owning worker. Cross-worker ops must use `csilk_dispatch(ctx, cb, arg)`.
- Client recycling uses generation tags (`_csilk_client_recycle_dispatch_cb`); destruction verifies `client->generation == gen && client->state == CSILK_CONN_CLOSING` to prevent ABA/UAF.
- `csilk_mq_t` teardown is deferred until **after** all worker threads are joined.

### Streaming backpressure

`csilk_response_write()`, `csilk_sse_send()`, `csilk_ws_send()` enforce per-connection watermarks:
- Return `0` → high watermark (`write_high_water_mark`, default 64 KB) reached: pause producer, resume on `csilk_on_drain()`.
- Return `-1` → queue overflow or error.

### Key module dependency graph

Arrows point from dependent → dependency (`A ──→ B` = A links against B):
```
workflow ──→ { core, json, ai, mq, wasm, yaml }
http       ──→ { core, json, tls, http2, mq, zlib, llhttp }
http2      ──→ { core, tls, nghttp2 }
db         ──→ { core, sqlite3, curl, mysql, pq, hiredis, mongoc }
ai         ──→ { core, json, curl }
tls        ──→ { core, openssl }
mq         ──→ core
wasm       ──→ core
bypass     ──→ core
core       ──→ { json, openssl, threads, {libuv|liburing}, yaml, m }
json       ──→ yyjson
umbrella   ──→ { core, json, tls, mq, http2, http, db, ai, workflow, wasm, bypass }
```

## Key Directories

| Path | Purpose |
|---|---|
| `src/core/` | HTTP server scaffolding, arena, config, ctx, router, logger, sync, IO abstractions |
| `src/core/uring/` | io_uring event loop, connection, server, thread pool |
| `src/core/http/` | HTTP/1.1 server, connection, TLS, router, app, swagger |
| `src/core/primitives/` | Arena, bounded buffer, KV store, URL decoding |
| `src/core/config/` | YAML config, logger, lifecycle hooks, admin dashboard |
| `src/core/ctx/` | Request context, async lifecycle, generation tracking |
| `src/core/json/` | JSON engine sources (wraps yyjson) |
| `src/core/cache/` | MVCC cache for DB-abstraction layer |
| `src/core/plugin/` | WASM VM & WASI sandbox plugin engine |
| `src/core/io/` | AF_XDP & DPDK kernel-bypass drivers, IO perf probes |
| `src/core/server/` | Multi-worker server lifecycle, RCU router swap, driver injection, shutdown, worker pool |
| `src/core/internal/` | Internal-only headers (header_map, query, lfqueue) not exposed in include/ |
| `src/crypto/` | base64, sha1, bcrypt, blowfish sboxes, cipher/crypto dispatch |
| `src/drivers/db/` | SQLite, MySQL, PostgreSQL, Redis, MongoDB drivers |
| `src/drivers/ai/` | OpenAI, Ollama, DeepSeek LLM client drivers |
| `src/drivers/cipher/` | OpenSSL-backed AES-256-GCM, RSA-OAEP, RSA-PSS |
| `src/drivers/vector/` | HNSW SIMD vector index (AVX2 cosine/L2/dot) |
| `src/middleware/` | auth, cors, csrf, jwt, ratelimit, sliding-ratelimit, session, sse, metrics, otlp, waf, circuit-breaker, grpc-gateway, gzip |
| `src/protocols/` | WebSocket (RFC 6455), HTTP/2 (nghttp2), HTTP/3 (QUIC), swagger, MCP |
| `src/messaging/` | MQ core, pubsub, WAL, raft RPC, consensus, snapshot |
| `src/workflow/` | Agent engine, DAG scheduler, DSL parser, WAL, trace |
| `src/app/` | High-level app API, groups, routes, admin handlers |
| `src/reflection/` | Reflection marshal/unmarshal/free (struct binding for JSON) |
| `src/util/` | CPU flame graph (backtrace sampling) |
| `include/csilk/` | Public headers mirroring src/ layout (opaque handles only) |
| `fuzz/` | libFuzzer harnesses: fuzz_test, fuzz_url, fuzz_yaml, fuzz_headers + corpus + dict |
| `tests/` | Unit, integration, stress, and property-based tests per module |
| `python/` | CFFI/ctypes bindings + setup.py + Python unit tests |
| `examples/` | Example apps: basic, ai, database, websocket, middleware, advanced |
| `cmake/` | CMake modules: targets, sources, tests, options, dependencies, tooling, install, examples |
| `docs/` | English (en/) and Chinese (zh-CN/) docs, superpowers guide, doxy headers |
| `share/` | Swagger UI assets, embedded APM dashboard HTML |

## Development Commands

### Configure & build

```bash
# Minimal configure (Release, system compiler)
cmake -B build -S .

# Debug build with clang (recommended for development)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DCSILK_BUILD_SHARED=ON -DENABLE_OOM_TEST=ON

# Build all targets (libraries + tests + examples)
cmake --build build -j$(nproc)

# With native host CPU tuning (-march=native) for benchmarks/profiling
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCSILK_ENABLE_NATIVE_ARCH=ON
```

### Test

```bash
# Unit tests only (excludes integration families)
ctest --test-dir build -E test_integration --timeout 30 --output-on-failure

# Single test
ctest --test-dir build -R test_bcrypt --timeout 10 --output-on-failure

# Integration tests
ctest --test-dir build -R test_integration --output-on-failure

# Extended-timeout tests (configured in CMakeLists.txt)
# test_timeout: 10s | test_multi_worker, test_mq_concurrent: 30s
# test_ws_concurrent, test_sse_concurrent: 15s
```

Test categories in `cmake/tests.cmake`: core, app, workflow, middleware, protocols, security, drivers (DB/AI), reflection, messaging, and integration families (`test_integration`, `test_sse_integration`, `test_session_integration`, `test_middleware_chain_integration`, `test_openapi_integration`, `test_admin_integration`).

### Sanitizer builds

```bash
# ASAN (address + leak, Linux only; mutually exclusive with TSAN)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_ASAN=ON -DENABLE_OOM_TEST=ON -DCSILK_BUILD_SHARED=ON

# TSAN (data-race detection, mutually exclusive with ASAN)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_TSAN=ON -DENABLE_OOM_TEST=ON

# Coverage (requires gcc; TEST_OOM must be OFF)
cmake -B build -S . -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_COVERAGE=ON -DENABLE_OOM_TEST=OFF

# libFuzzer (requires clang-19+)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang-19 \
  -DUSE_ASAN=ON -DUSE_FUZZER=ON
cmake --build build --target fuzz_test fuzz_yaml fuzz_url fuzz_headers
```

Runtime flags to mirror CI (suppression files under `cmake/`):

```bash
# ASAN
ASAN_OPTIONS="detect_leaks=1:symbolize=1:abort_on_error=1" \
LSAN_OPTIONS="suppressions=$PWD/cmake/.lsan-suppressions" \
  ctest --test-dir build -E test_integration --output-on-failure

# TSAN
TSAN_OPTIONS="halt_on_error=1:print_suppressions=0:suppressions=$PWD/cmake/.tsan-suppressions" \
  ctest --test-dir build -E test_integration --output-on-failure

# Coverage post-run
gcovr -r build --filter src/   # excludes flamegraph.c, redis_storage.c, workflow_debug.c, uring_vector.c
```

### io_uring backends (Linux)

Two modes — the CI job exercises both:

```bash
# (a) Native io_uring event loop
cmake -B build_uring -S . -DCMAKE_BUILD_TYPE=Debug -DCSILK_USE_URING=ON
cmake --build build_uring -j$(nproc)
ctest --test-dir build_uring --timeout 30 --output-on-failure

# (b) libuv backend delegating to io_uring via environment variable
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCSILK_BUILD_SHARED=ON
UV_USE_IO_URING=1 ctest --test-dir build --output-on-failure
```

### Lint & format

```bash
cmake --build build --target check-format   # dry-run; fails on mismatch
cmake --build build --target format         # apply clang-format in-place
cmake --build build --target tidy           # clang-tidy (WarningsAsErrors)
./scripts/check_version_sync.sh             # CMake version ↔ git tag
```

`clang-format` is applied (via `cmake/format.cmake`, `GLOB_RECURSE`) to `src/**`, `include/**`, `tests/**`, and `examples/**`. Run `format` before committing — the tree was normalized in Aug 2026 and every new file must match.

## Code Conventions & Common Patterns

### Naming

- **Public functions**: `csilk_` prefix, camelCase — e.g., `csilk_response_write`, `csilk_sse_send`, `csilk_dispatch`.
- **Internal helpers**: `_csilk_` prefix — e.g., `_csilk_send_response`, `_csilk_next_handler`.
- **Struct tags**: `csilk_*_t` — e.g., `csilk_ctx_t`, `csilk_server_t`. Opaque handles for external use; concrete `struct csilk_*_s` lives in `src/` internals.
- **Constants**: `CSILK_` prefix, SCREAMING_SNAKE — e.g., `CSILK_BCRYPT_HASH_LEN`, `CSILK_MAX_PARAMS`.
- **Tests**: `test_*` prefix — e.g., `test_router`, `test_hot_reload_stress`.

### Error handling

- Functions return `int`: `0` on success, negative errno-style codes on failure.
- Out-of-memory paths return `-1` (or specific errno macro). Callers MUST check return values.
- `csilk_panic(ctx)` unwinds via deferred callbacks (LIFO) registered with `csilk_ctx_defer()`. After panic, the recovery middleware sends HTTP 500.
- Response errors use `csilk_abort(c, status, body)` to short-circuit the handler chain.

### Async & threading

- **Never call raw `uv_*` or `pthread_*`** in cross-backend code. Use `csilk_io_*`, `csilk_thread_*` (`csilk_thread_create`/`join`/`self`/`setaffinity`), `csilk_barrier_*` (`csilk_barrier_init`/`wait`/`destroy`), and `csilk_mutex_t`/`csilk_cond_t` from `<csilk/core/sys_io.h>` and `<csilk/core/sync.h>`.
- `wp->active_clients` is strictly single-thread-confined. Cross-worker ops must use `csilk_dispatch(ctx, cb, arg)`.
- `uv_barrier_t` (and `csilk_barrier_t` underneath) must be heap-allocated — never stack-allocated when used across threads.
- Client recycling uses generation tags; verify `client->generation == gen && client->state == CSILK_CONN_CLOSING` before destroying.
- Server teardown order: drain active clients → drain timers → drain `wp->dispatch_async` queues → join workers → stop hot-reload → EBR grace period → destroy router → close event loop → free pools. MQ teardown is deferred until after all joins.

### Arena & context patterns

- Prefer `csilk_arena_alloc()` (uninitialized, fastest) and `csilk_arena_calloc()` (zeroed) over `malloc`/`free`.
- For heap objects that outlive the request arena, use `csilk_set_ex(c, key, ptr, destructor)` for RAII cleanup on arena reset.
- String views (`csilk_view_t`: `const char *data; size_t len;`) distinguish zero-copy parser buffers from arena NUL-terminated strings.

### Router

- Segments can be static, parameterized (`:id`), or wildcard (`*`).
- Router swaps are serialized under `config_mutex`; readers are lock-free.
- Wildcard parameters are backtracked on method mismatch or handler failure.

### TLS / HTTPS

- TLS 1.3 is mandatory in production. OpenSSL must be ≥ 1.1.1.
- SSL read buffers are arena-allocated to keep decrypted data valid for zero-copy views.

### Hot reload

- `csilk_dev_hot_reload_trigger()` swaps the router at runtime without restart.
- Temp library copies use `mkstemp(0600)`; OOM must roll back all of `new_router`, `new_handle`, `tmp_path`.

### Middleware ordering

Middleware executes in registration order. Common pattern:

```c
csilk_app_t *app = csilk_app_new();
csilk_use(app, csilk_recovery_middleware());     // outermost: catches panics
csilk_use(app, csilk_logger_middleware());
csilk_use(app, csilk_jwt_middleware(secret));
csilk_use(app, csilk_cors_middleware(...));
csilk_use(app, csilk_ratelimit_middleware(...));
csilk_use(app, csilk_csrf_middleware(...));
csilk_use(app, csilk_waf_middleware(...));
// handler-specific middleware here
csilk_get(app, "/api/echo", echo_handler);
```

## Important Files

| Path | Role |
|---|---|
| `CMakeLists.txt` | Top-level orchestrator; pulls in cmake/*.cmake modules |
| `cmake/options.cmake` | Single source of truth for version (`CSILK_VERSION_MAJOR/MINOR/PATCH`) and build options |
| `cmake/targets.cmake` | All library/executable target definitions + alias setup |
| `cmake/sources.cmake` | Source file lists per modular target |
| `cmake/tests.cmake` | Test executable registration + per-module grouping |
| `cmake/tooling.cmake` | clang-format, clang-tidy, doxygen, coverage targets |
| `cmake/install.cmake` | `make install` rules, CMake package config, pkg-config `.pc` files, CPack |
| `cmake/format.cmake` | clang-format file-glob and apply logic |
| `cmake/.lsan-suppressions` | Leak-suppress rules used by CI ASAN runs |
| `cmake/.tsan-suppressions` | Race-suppress rules used by CI TSAN runs |
| `.clang-tidy` | Suppressed false positives (blowfish uninitialized assign) |
| `.clang-format` | Formatter config applied project-wide |
| `.gcovr` | Coverage report exclusions |
| `include/csilk.h` | Umbrella public header |
| `include/csilk/version.h.in` | Version string header template |
| `src/core/server/connection.c` | Core connection state machine; handles client pool, lifecycle, generation tags |
| `src/core/server/server_lifecycle.c` → split into `server_rcu.c` + `server_driver.c` | Server startup, RCU router swap, driver injection |
| `src/core/server/server_shutdown.c` | Ordered teardown path |
| `src/middleware/jwt.c` | JWT HS256 middleware + validation policies |
| `src/middleware/waf.c` | eBPF XDP dynamic WAF + OTLP APM ring buffer |
| `src/drivers/db/` | Database abstraction + SQLite/MySQL/PQ/Redis/Mongo drivers |
| `src/drivers/vector/hnsw.c` | HNSW skip-graph index with AVX2 SIMD distance kernels |
| `src/reflection/reflect.c` | Reflection marshal/unmarshal for struct↔JSON binding |
| `scripts/check_version_sync.sh` | Verifies CMake version matches git tag; `--expected <ver>` for release validation |
| `scripts/csilkskel` | Interactive project scaffold generator (C API + CMake + test skeleton) |
| `scripts/tag-release.sh` | Bumps version across all 18 locations (src/, python/, cmake/, docs, ASCII diagrams) |
| `scripts/profile.sh` | CPU profiler harness (perf, flamegraph) |
| `scripts/run_benchmarks.sh` | Benchmark runner; compares against prior results via `compare_benchmarks.py` |
| `scripts/clean_builds.sh` | Lists CMake build trees and prunes all but the canonical set (dry-run default, `--prune` to delete, `--keep a,b,c` to override) |
| `scripts/generate-sdk.py` | TypeScript/Python SDK generator from OpenAPI spec (`--url` or `--file`, `--lang typescript|python|both`) |
| `scripts/setup_bench_tools.sh` | Benchmark toolchain installer |
| `fuzz/fuzz.dict` | libFuzzer dictionary for fuzz_test and fuzz_headers harnesses |
| `fuzz/corpus/` | Seed corpora for each fuzzer |
| `share/csilk/apm_ui.html` | Embedded single-page Web APM dashboard served at `/admin/apm` |
| `share/swagger-ui/` | Swagger UI assets served at `/swagger` |
| `docs/en/` | English documentation: getting-started, architecture, design, performance-tuning, troubleshooting, user-manual, contributing |
| `docs/zh-CN/` | Chinese mirror of the above |

## Runtime & Tooling Preferences

- **CMake** ≥ 3.11 is the build system; no other build backend is supported.
- **ccache** is auto-detected and used as compiler launcher if available.
- **System dependencies** (must be installed separately): libyaml-dev, libssl-dev (≥ 1.1.1), zlib1g-dev, libcurl4-openssl-dev (≥ 7.80.0), libsqlite3-dev. libpq-dev, libmysqlclient-dev, hiredis, libmongoc are optional per-driver.
- **Auto-fetched** via FetchContent: libuv (or liburing), nghttp2, llhttp, yyjson, cJSON.
- **Compiler**: Clang 19+ is default; GCC 13+ for coverage jobs. Apple Clang and MSVC are rejected.
- **Shared libs**: `.so` on Linux, `.dylib` on macOS. Always built alongside `.a`.
- **pkg-config**: `pkg-config --cflags --libs csilk` works after install.

## Testing & QA

### Test categories

| Category | Filter | Timeout | Notes |
|---|---|---|---|
| Unit (core, app, middleware, protocols, security, drivers, reflection, messaging) | `-E test_integration` | 10s | Core property tests (crypto, bcrypt, UUID) use `/dev/urandom` salts — assert-hash tests are `#ifdef TEST_OOM`-guarded |
| Integration | `-R test_integration` | 30s | Live HTTP server fixture; also SSE, session, middleware-chain, OpenAPI, admin |
| Stress | `test_*_stress` | 30–60s | Multi-worker, RCU reader storm, hot-reload ramp |
| Fuzz | `fuzz_*` | Manual (CI: 60–120s) | `fuzz_test`, `fuzz_url`, `fuzz_yaml`, `fuzz_headers`; corpus in `fuzz/corpus/`; dict at `fuzz/fuzz.dict` |

### Coverage

Built with `-DUSE_COVERAGE=ON` under gcc (ASAN/TSAN incompatible). GCOV excludes `flamegraph.c`, `redis_storage.c`, `workflow_debug.c`, `uring_vector.c` via `.gcovr`. Current reported coverage: **69%** (12 609 / 18 281 lines). Target: increase via integration tests (SSE alone drove sse.c from 22% → 77%; the coverage campaign further raised websocket.c → 85%, gzip.c → 68%, bounded_buf.c → 100%).

### CI matrix (`.github/workflows/ci.yml`)

- **test**: ubuntu-24.04 + macos-14 × Debug + Release; ASAN on Linux Debug; Python bindings on Release or Linux.
- **lint**: clang-format check, clang-tidy, version-sync, Mermaid-diagram validation.
- **fuzz**: clang-19 + ASAN + libFuzzer; 4 harnesses, each with corpus replay.
- **arm64**: cross-compile via `aarch64-linux-gnu-gcc`.
- **io_uring**: libuv+`UV_USE_IO_URING=1` and native `CSILK_USE_URING=ON`.
- **tsan**: separate TSAN job with `halt_on_error=1`.
- **coverage**: gcc + gcovr job.
- **benchmarks**: native-arch Release runs; uploads results.
- **release**: version bump + tag + PyPI wheel + GitHub release.

## Commit Convention

```
type(scope): 🎯 subject
```

Types with emojis: `feat ✨`, `fix 🐛`, `docs 📝`, `style 🎨`, `refactor ♻️`, `test ✅`, `build 📦`, `ci 👷`, `chore 🧹`. Emoji goes **after** the colon, one per commit, subject lowercase imperative mood.

## Versioning

Version is pinned in `cmake/options.cmake` as a single source of truth (`CSILK_VERSION_MAJOR`/`MINOR`/`PATCH`). Bump via `scripts/tag-release.sh` which updates all 18 version locations across `src/`, `python/`, `cmake/`, docs, and ASCII diagrams. Sync check: `./scripts/check_version_sync.sh` (or `--expected <ver>` for release validation).
