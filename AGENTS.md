# Repository Guidelines — csilk

A lightweight HTTP web framework written in C23, delivering **P99 latency ≤ 5ms under 10K QPS** on commodity hardware. Built on libuv (default) or io_uring (Linux-only opt-in), llhttp, nghttp2, and yyjson.

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
- **Dual-variant verification**: any change touching `cmake/`, `src/core/`, `src/app/`, `src/middleware/`, `src/protocols/`, or `include/csilk/` **must** compile cleanly and pass `ctest` in **both** Core-Only (`-DCSILK_BUILD_FULL=OFF`) and Full (`-DCSILK_BUILD_FULL=ON`) builds.
- **Run tests via `ctest`**: never `make test` or invoke test executables directly. Use `--test-dir build` (or the variant dir) and filter with `-R` / `-E`.
- **ASAN/TSAN runtime**: mirror CI flags (`ASAN_OPTIONS`, `LSAN_OPTIONS`, `TSAN_OPTIONS`) when running tests under sanitizers locally. Suppressions live under `cmake/`.
- **Timeout-sensitive tests**: `test_timeout` (10s), `test_multi_worker`/`test_mq_concurrent`/`test_mcp_stdio_pool` (30s), `test_ws_concurrent`/`test_sse_concurrent` (15s), AI/vector mocks (`test_openai_mock`, `test_ai_anthropic`, `test_ollama_mock`, `test_vector_milvus_qdrant_mock`, `test_agent_memory`, 60s), `test_cmake_package_target_split` (180s). Pass explicit `--timeout` budgets on CTest invocations.
- **OOM/deterministic tests**: wrap hash-equality assertions in `#ifdef TEST_OOM`; enable with `-DENABLE_OOM_TEST=ON`.

### Benchmarking & profiling

- **Native CPU tuning**: benchmarks and profiling builds must use `-DCSILK_ENABLE_NATIVE_ARCH=ON` to append `-march=native`.
- **Flame graphs**: `src/util/flamegraph.c` provides backtrace sampling; excluded from coverage by `.gcovr`.
- **Run benchmarks**: `scripts/run_benchmarks.sh`; compare against prior results with `scripts/compare_benchmarks.py`.

### Fuzzing

- **Build fuzzers**: Core harnesses (`fuzz_test fuzz_yaml fuzz_url fuzz_headers fuzz_h2 fuzz_websocket`) build in Core-Only mode; optional harnesses (`fuzz_raft_rpc fuzz_mcp fuzz_sql_rewrite fuzz_wasm fuzz_h3`) require `-DCSILK_BUILD_FULL=ON` (or their respective `CSILK_ENABLE_*` flag) plus `clang-19` + ASAN.
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

- **Build full shared lib first**: `cmake --build build_full --target csilk_shared` (bindings map full optional C symbols).
- **Run full CI scripts**: export `LIBCSILK_PATH=$PWD/build_full/libcsilk.so` and `PYTHONPATH=$PWD/python`, then run `test_csilk.py`, `test_abi_layout.py` (with `CSILK_ABI_PROBE_BIN="$PWD/build_full/test_python_abi" CSILK_REQUIRE_ABI_PROBE=1`), `test_middleware_bindings.py`, `test_test_context_compat.py`, and `test_asyncio_support.py` under `python/tests/`.
- **Package**: `python/setup.py` drives PyPI wheel generation.

### Git workflow

- **Commit convention**: `type(scope): 🎯 subject` with emoji after colon (e.g., `feat(server): 🔥 add RCU router swap`), lowercase imperative subject. See full type list in this document.
- **Pre-commit gates**: run `format`, `check-format`, `bash ./scripts/check_sync_primitives.sh`, `bash ./scripts/check_abi_compatibility.sh build_full/libcsilk.so`, `./scripts/check_version_sync.sh`, `python3 scripts/check_mermaid.py .`, and confirm `ldd build_core/libcsilk.so` has zero `sqlite`/`curl` dependencies.
- **Clean history**: split multi-concern work into focused logical commits; never `--amend` published history and never auto-push without explicit user instruction.

### Prohibited patterns

- **Never** include `<pthread.h>` or call raw `pthread_*` anywhere under `src/` (`scripts/check_sync_primitives.sh` fails CI on any match); never call raw `uv_*` directly in cross-backend code. Always use `csilk_io_*`, `csilk_thread_*`, `csilk_barrier_*`, `csilk_mutex_t`/`csilk_cond_t`.
- **Never** declare `uv_barrier_t` or `csilk_barrier_t` on the stack when used across threads — always `calloc`/`free`.
- **Never** use `csilk_arena_alloc()` when zero-initialized memory is required — use `csilk_arena_calloc()`.
- **Never** call `client_destroy()` from a non-owner worker thread — enqueue a recycling task via `csilk_dispatch()`.
- **Never** include `messaging/mq_internal.h` transitively through `include/csilk/core/internal.h` — add explicit includes only where needed.
- **Never** pull `SQLite3::SQLite3` or `CURL::libcurl` into Web Core (`CSILK_BUILD_FULL=OFF`), add direct optional-module teardown calls into `server_lifecycle.c`, or add simulated production stubs.
- **Never** assert bcrypt/hash equality without `#ifdef TEST_OOM` guard (tests use `/dev/urandom` salts by default).

### Declaration placement

- **All `#include` directives** must appear at the top of the file, before any function definition or global variable declaration. Conditional includes (`#ifdef HAS_X` → `#include <x.h>`) are allowed only within the top include block, before the first function body.
- **All global/file-scope `static` variable declarations** (including `static _Thread_local`, `static _Atomic`, and `static const` table/array) must be placed in the top declaration block, i.e., after `#include` directives and typedefs but **before** the first function definition (including `static inline` functions). Do not sprinkle static variables between or after function bodies.
- **Exception**: `#include` inside a top-level `#ifdef` guard that protects an entire function group (e.g. `#ifdef CSILK_HAS_NGTCP2 … #endif` wrapping multiple function bodies) is permissible when the guarded functions use types from that header; move the `#include` into the top block if no guarded function exists.

**Rationale**: Consistent declaration placement enables fast IDE/clangd navigation, makes the file’s global state surface at the top for review, and prevents accidental ODR issues from late declarations. It also makes code-review diffs cleaner: new globals are visible at the top of the diff rather than buried mid-file.

### Module Boundary & Dependency Constraints (Web Core vs Optional Modules)

- **Zero-bloat Web Core**: `CSILK_BUILD_FULL` defaults to `OFF`. Core targets (`base`, `json`, `crypto`, `reflection`, `runtime`, `tls`, `http2`, `http`, `protocols`, `middleware`, `app`) must build standalone with zero `SQLite3` or `libcurl` linkage (`ldd build_core/libcsilk.so | grep -qiE "sqlite|curl"` must return non-zero).
- **Strict downward dependency**: Web Core files (`src/core/`, `src/app/`, `src/middleware/`, `src/protocols/`) must not call optional-module symbols (`db`, `mq`, `ai`, `vector`, `mcp`, `workflow`, `grpc`, `wasm`, `bypass`, `perm`, `http3`) unconditionally — guard callers cleanly behind `#ifdef CSILK_HAS_<MODULE>` or register hooks/callbacks at runtime.
- **Extensible server teardown**: per-server optional or stateful subsystems (`session`, `ws_room`, `otlp_trace`, `h3`, `mq`, etc.) must register a `csilk_server_teardown_fn` via `_csilk_server_register_teardown()` (executed LIFO on shutdown, max 16 slots in `srv_internal.h`) instead of hardcoding subsystem teardown calls in `server_lifecycle.c`.
- **Single option model**: the 8 legacy `src/plugins/*` wrappers were retired in favor of CMake optional module options (`CSILK_ENABLE_*` in `cmake/options.cmake`) with transitive dependency enforcement (`WAMR -> WASM`, `MCP -> AI`, `WORKFLOW -> AI + MQ + WASM`).

### ABI & Public API Governance (SOVERSION 2)

- **Export whitelist**: `cmake/libcsilk.map` restricts dynamic exports strictly to `csilk_*` (`global: csilk_*; local: *;`). Never invent non-`csilk_` public symbols; internal cross-TU symbols must use `_csilk_` and be kept `static` whenever single-TU.
- **Opaque handle encapsulation**: `include/csilk/` headers expose opaque pointer typedefs (`csilk_*_t`); concrete `struct csilk_*_s` layouts live in `src/` and internal headers under `src/core/internal/` must never leak into public headers.
- **ABI baseline gate**: `bash ./scripts/check_abi_compatibility.sh build_full/libcsilk.so` checks exports against `cmake/abi_baseline_symbols.txt` (`SOVERSION 2`). Never remove or break existing baseline symbols within a major `SOVERSION`; intentionally adding/removing exported API in Full mode requires regenerating `cmake/abi_baseline_symbols.txt`.
- **Python struct layout parity**: any edit to public structs mirrored by `ctypes` in `python/csilk/` must also update `tests/core/test_python_abi.c` and pass `test_abi_layout.py` with `CSILK_REQUIRE_ABI_PROBE=1`.

### Honest Implementation & Fail-Closed Discipline

- **No fake drivers or stub toggles**: simulated `dpdk_pmd`, fake eBPF `xdp_waf`, simulated `llama.c`, dead `quic_vtable`, dead `csilk_h3_listener_*`, `af_xdp` simulation rings (`sim_fr_*`/`sim_cr_*`), `wasm` simulated execution/fuel stubs, fake WASI stdout stubs, and `CSILK_ENABLE_EXPERIMENTAL_STUBS` have been removed. Every public driver/middleware entrypoint must represent real functionality (`session_store` local backend uses a real TTL in-memory hash table) or explicit capability absence.
- **Fail closed on absent runtimes**: optional native integrations (e.g., `HAS_WAMR` in `wasm_vm.c`/`wasm_plugin.c`/`wasm_wasi.c`, `CSILK_HAS_NGTCP2`/`nghttp3` in HTTP/3 transport, Linux `AF_XDP` in `af_xdp.c`, optional DB drivers) must set `errno = ENOTSUP` (or OS socket errno) and return `-1` / `nullptr` when unconfigured or unavailable.
- **No parallel duplicate subsystems**: place shared WAL file I/O helpers in `src/modules/common/wal_common.h`/`.c` (reused by `workflow_wal.c`), keep both fixed and sliding rate-limiters in `src/middleware/ratelimit.c`, unify `session_store.c` local/MQ in-memory tables, unify `include/csilk/csilk.h` (with `CSILK_HAS_*` guards and `include/csilk.h` forwarding), and keep dynamic WAF rules/filtering (`csilk_waf_filter_*`) unified in `src/middleware/waf.c`.

### Per-Server Isolation, Security & Hot-Path Constraints

- **Per-server isolation over global singletons**: bind mutable runtime state (sessions, WS rooms, OTLP tracer, H3/MQ/MCP state) to `csilk_server_t` or explicit handles so multiple servers in one process stay isolated and reentrant.
- **Zero bloated static `.bss` tables**: large hash/override tables (`ip_table`/`key_table` in `ratelimit.c`, `s_test_overrides`/`s_secret_cache` in `secret.c`) must be lazily heap-allocated on first use via `csilk_once` and cleaned up on exit (`atexit`), not statically reserved in `.bss`.
- **Input, path & SSRF validation**: reject `..` path traversal, NUL/control characters, and non-HTTP(S) URI schemes across static file serving, `.so` plugin loading (`loader.c`), and outbound HTTP drivers (`ssrf.c`).
- **Hot-path allocation discipline**: keep request parsing, header indexing, routing, and response serialization on `c->arena` + zero-copy `csilk_str_view_t`; avoid per-request heap `malloc`/`free` churn on the hot path.
- **Toolchain & static-analysis compliance**: new code must build cleanly under both GCC 13+ and Clang 19+ and pass `tidy` (`.clang-tidy` enforces `WarningsAsErrors: '*'`). Prefer `enum` constants over C23 `constexpr` when cross-compiler header compatibility requires it.

### CMake, Packaging & Workspace Hygiene

- **Guard optional CTest properties**: always wrap `set_tests_properties(<name> ...)` for optional tests inside `if(TEST <name>)` in `CMakeLists.txt`.
- **Drift-free test registration**: prefer single-list `foreach` loops for uniform test directories in `cmake/tests.cmake`; when using `ZIP_LISTS` for mixed directories, maintain exact 1-to-1 list parity.
- **Package contract synchronization**: any added/removed module must stay synchronized across `cmake/targets.cmake`, `cmake/install.cmake` (`CSILK_INSTALL_TARGETS`, `.pc` variables), `cmake/csilk-config.cmake.in` (`csilk::<mod>`, `csilk::<mod>_shared`, `csilk::shared`), and `tests/cmake/test_package.cmake`.
- **Workspace hygiene**: never leave temporary test outputs (`*.db`, `*.yaml`, `*.wal`) or redundant `build_*` trees tracked or littered across the repository root.

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
| `csilk_client_t` | `src/core/server/connection_{pool,state,timer,close,io}.c` | Connection lifecycle: state machine (INIT→ACCEPTED→TLS→READING→PROCESSING→WRITING→STREAMING→CLOSING→CLOSED), generation tag, pending I/O counters |
| `csilk_router_t` | `include/csilk/core/router.h` + opaque impl | Segment-prefix trie with RCU/swappable readers |
| `csilk_app_t` | `include/csilk/app.h` | High-level app builder (hooks, groups, middleware registration) |
| `csilk_mq_t` | `include/csilk/messaging/mq.h` | Asynchronous thread-safe message queue with PubSub and WAL |
| `csilk_vector_db_t` | `include/csilk/drivers/db/vector.h` | HNSW skip-graph vector index engine |
| `csilk_db_stmt_t` | `include/csilk/drivers/db.h` | Native prepared statement with typed parameter bindings |
| `csilk_migration_t` | `include/csilk/drivers/db/migration.h` | Flyway-style database migration engine with distributed locks |
| `csilk_ai_trace_t` | `include/csilk/drivers/ai.h` | End-to-end AI latency, token throughput, and tool span tracing |
| `csilk_ai_react_loop_t` | `include/csilk/app/workflow.h` | Multi-step tool-calling ReAct agent execution loop |
| `csilk_ai_skill_t` | `include/csilk/drivers/ai/skill.h` | Agent Skill abstraction (L1/L2/L3 progressive disclosure); LOCAL via bounded fork/execvpe, REMOTE via csilk_mq |
| `csilk_tool_registry_t` | `include/csilk/drivers/ai/tool_registry.h` | Centralized tool definition, schema generation & dispatch |
| `csilk_mcp_stdio_pool_t` | `include/csilk/protocols/mcp.h` | Reusable Model Context Protocol stdio client process pool |
| `csilk_secret_provider_t` | `include/csilk/core/secret.h` | Tiered cloud-native secret provider with circuit breakers |


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
- Teardown of `csilk_mq_t` and per-server optional subsystems (`session`, `ws_room`, `otlp_trace`, `h3`, etc.) executes via registered `_csilk_server_register_teardown()` LIFO hooks **after** all worker threads are joined.

### Streaming backpressure

`csilk_response_write()`, `csilk_sse_send()`, `csilk_ws_send()` enforce per-connection watermarks:
- Return `0` → high watermark (`write_high_water_mark`, default 64 KB) reached: pause producer, resume on `csilk_on_drain()`.
- Return `-1` → queue overflow or error.

### Key module dependency graph

Web Core (4 layered physical libraries `base`, `runtime`, `http`, `app` + 8 compatibility aliases for 11 logical modules, always built with `-DCSILK_BUILD_FULL=OFF`, zero SQLite3/libcurl dependencies) forms the base layer; optional domain modules (`CSILK_ENABLE_*` / `-DCSILK_BUILD_FULL=ON`) depend strictly downward without Web Core up-references (optional integrations register via runtime registries in `obs_registry.h`). Arrows point from dependent → dependency (`A ──→ B` = A links against B):
```
# Web Core (always built — 4 physical layer libraries + 8 compat aliases)
app           ──→ { http, protocols, middleware, reflection }
middleware    ──→ { http, runtime, json, tls, zlib, openssl, threads }
protocols     ──→ { http, http2, runtime, json, reflection }
http          ──→ { runtime, json, tls, http2, zlib, openssl, llhttp }
http2         ──→ { runtime, tls, nghttp2 }
tls           ──→ { runtime, openssl }
runtime       ──→ { base, crypto, json, reflection, openssl, threads, {libuv|liburing}, yaml, m }
reflection    ──→ { base, json }
crypto        ──→ { base, openssl, threads }
json          ──→ yyjson
base          ──→ { threads, {libuv|liburing}, m }

# Optional Domain Modules (CSILK_ENABLE_* = OFF by default, ON via -DCSILK_BUILD_FULL=ON)
observability ──→ { app, middleware, http, runtime, json, threads }
workflow      ──→ { app, http, runtime, json, ai, mq, wasm, yaml, m }
mcp           ──→ { protocols, http, runtime, json, ai, curl }
grpc          ──→ { middleware, http, runtime, json }
h3            ──→ { runtime, http, tls, ngtcp2, nghttp3, openssl }
db            ──→ { runtime, json, sqlite3, optional mysql/pq/hiredis/mongoc }
ai            ──→ { http, runtime, json, curl }
vector        ──→ { runtime, json, curl, m }
permission    ──→ { runtime, json }
mq            ──→ { http, runtime }
wasm          ──→ { app, runtime, optional vmlib }
bypass        ──→ runtime
umbrella      ──→ { Web Core + all enabled CSILK_ENABLE_* modules }
```

## Key Directories

| Path | Purpose |
|---|---|
| `src/core/` | HTTP server scaffolding, arena, config, ctx, router, logger, sync, IO abstractions |
| `src/core/uring/` | Optional `io_uring` event loop, connection, server, thread pool, buffer/sqpoll (`CSILK_USE_URING`) |
| `src/core/http/` | HTTP/1.1 server, connection, TLS, router, app, swagger |
| `src/core/primitives/` | Arena, bounded buffer, KV store, URL/query decoding, router, response |
| `src/core/config/` | YAML config, logger, tiered secret provider (`secret.c`), lifecycle hooks, hot reload |
| `src/core/ctx/` | Request context, async lifecycle, generation tracking |
| `src/core/json/` | JSON engine sources (wraps yyjson) |
| `src/core/plugin/` | Dynamic `.so` plugin loader (`loader.c`) |
| `src/core/server/` | Multi-worker server lifecycle, RCU router swap, driver injection, shutdown, worker pool |
| `src/core/internal/` | Internal-only headers (`header_map`, `query`, `lfqueue`, `srv_internal`, `srv_impl`, `obs_registry`) not exposed in include/ |
| `src/crypto/` | base64, sha1, bcrypt, blowfish sboxes, cipher/crypto dispatch, OpenSSL cipher backend (`cipher_openssl.c`) |
| `src/middleware/` | Web Core middleware: auth, cors, csrf, jwt, ratelimit (fixed + sliding window), session, sse, metrics, waf (`csilk_waf_filter_*`), circuit-breaker, gzip |
| `src/protocols/` | Web Core protocol extensions: WebSocket (`websocket.c`, `ws_room.c`), Swagger/OpenAPI |
| `src/app/` | Web Core high-level app API, groups, routes |
| `src/reflection/` | Reflection marshal/unmarshal/free (struct binding for JSON) |
| `src/modules/common/` | *(Optional)* Shared helper headers/WAL I/O (`curl_common.h`, `wal_common.h`/`.c`, `wal_uring.c`) |
| `src/modules/db/` | *(Optional `CSILK_ENABLE_DB`)* SQLite, MySQL, PostgreSQL, Redis, MongoDB drivers, prepared statements, savepoint scope, migrations |
| `src/modules/ai/` | *(Optional `CSILK_ENABLE_AI`)* OpenAI, Ollama, Anthropic LLM drivers, tool registry, AI tracing container |
| `src/modules/vector/` | *(Optional `CSILK_ENABLE_VECTOR`)* HNSW SIMD vector index (AVX2 cosine/L2/dot) & Qdrant/Milvus drivers |
| `src/modules/permission/` | *(Optional `CSILK_ENABLE_PERM`)* Pluggable RBAC/ReBAC permission manager & simple in-memory driver |
| `src/modules/mq/` | *(Optional `CSILK_ENABLE_MQ`)* MQ core/pubsub/WAL (`mq/`) and Raft consensus/RPC/snapshot/WAL (`raft/`) |
| `src/modules/workflow/` | *(Optional `CSILK_ENABLE_WORKFLOW`)* DAG scheduler, DSL parser, ReAct loop, Agent Skills, WAL, trace |
| `src/modules/mcp/` | *(Optional `CSILK_ENABLE_MCP`)* Model Context Protocol JSON-RPC, server, client, stdio pool |
| `src/modules/grpc/` | *(Optional `CSILK_ENABLE_GRPC`)* gRPC wire codec & HTTP/JSON transcoding gateway |
| `src/modules/http3/` | *(Optional `CSILK_ENABLE_HTTP3`)* HTTP/3 QUIC transport & session (`ngtcp2`/`nghttp3`) |
| `src/modules/wasm/` | *(Optional `CSILK_ENABLE_WASM`)* WASM VM & WASI sandbox plugin engine (`WAMR`) |
| `src/modules/observability/` | *(Optional `CSILK_ENABLE_OBSERVABILITY`)* OTLP exporter/trace, CPU flame graph (`flamegraph.c`), Admin/APM dashboard (`admin.c`) |
| `src/modules/csv/` | *(Optional `CSILK_ENABLE_CSV`)* RFC 4180 CSV parser & serializer (`csv.c`) |
| `include/csilk/` | Public headers mirroring src/ layout (opaque handles only; compatibility forwarding headers retained at legacy paths) |
| `fuzz/` | libFuzzer harnesses: fuzz_test, fuzz_url, fuzz_yaml, fuzz_headers, fuzz_h2, fuzz_websocket, fuzz_raft_rpc, fuzz_mcp, fuzz_sql_rewrite + corpus + dict |
| `tests/` | Unit, integration, stress, and property-based tests (Web Core under `core/`, `app/`, `middleware/`, `protocols/`, `crypto/`, `security/`, `reflection/`; optional modules under `modules/`) |
| `python/` | CFFI/ctypes bindings + setup.py + Python unit tests |
| `examples/` | Example apps: basic, ai, database, websocket, middleware, advanced |
| `cmake/` | CMake modules: targets, sources, tests, options, dependencies, tooling, install, examples |
| `docs/` | English (en/) and Chinese (zh-CN/) docs, superpowers guide, doxy headers |
| `share/` | Swagger UI assets, embedded APM dashboard HTML |

## Development Commands

### Configure & build

```bash
# Minimal Core-Only configure (Release, default CSILK_BUILD_FULL=OFF, zero SQLite/CURL)
cmake -B build_core -S . -DCMAKE_BUILD_TYPE=Release -DCSILK_BUILD_SHARED=ON

# Full Debug build with clang (enables all 12 optional domain modules + OOM tests)
cmake -B build_full -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DCSILK_BUILD_FULL=ON -DCSILK_BUILD_SHARED=ON -DENABLE_OOM_TEST=ON

# Build all targets (libraries + tests + examples)
cmake --build build_full -j$(nproc)

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
cmake --build build --target fuzz_test fuzz_yaml fuzz_url fuzz_headers fuzz_h2 fuzz_websocket fuzz_raft_rpc fuzz_mcp fuzz_sql_rewrite
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
cmake --build build_full --target check-format             # dry-run; fails on mismatch
cmake --build build_full --target format                   # apply clang-format in-place
cmake --build build_full --target tidy                     # clang-tidy (WarningsAsErrors)
bash ./scripts/check_sync_primitives.sh                    # ban raw pthread_* in src/
bash ./scripts/check_abi_compatibility.sh build_full/libcsilk.so  # SOVERSION 2 symbol gate
./scripts/check_version_sync.sh                            # CMake version ↔ git tag
python3 scripts/check_mermaid.py .                         # validate Mermaid blocks
```

`clang-format` is applied (via `cmake/format.cmake`, `GLOB_RECURSE`) to `src/**`, `include/**`, `tests/**`, and `examples/**`. `.clang-format` enforces LLVM base, 4-space indentation, 100-column limit, `PointerAlignment: Left`, `BreakBeforeBraces: Linux`, `InsertBraces: true`, and `AlwaysBreakAfterReturnType: AllDefinitions`. Run `format` before committing — every modified or new C/H file must match.

## Code Conventions & Common Patterns

### Naming

- **Public functions**: `csilk_` prefix, snake_case — e.g., `csilk_response_write`, `csilk_sse_send`, `csilk_dispatch`.
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

- **Never include `<pthread.h>` or call raw `pthread_*`** anywhere in `src/`, and **never call raw `uv_*`** directly in cross-backend code. Use `csilk_io_*`, `csilk_thread_*` (`csilk_thread_create`/`join`/`self`/`setaffinity`), `csilk_barrier_*` (`csilk_barrier_init`/`wait`/`destroy`), and `csilk_mutex_t`/`csilk_cond_t` from `<csilk/core/sys_io.h>` and `<csilk/core/sync.h>`.
- `wp->active_clients` is strictly single-thread-confined. Cross-worker ops must use `csilk_dispatch(ctx, cb, arg)`.
- `uv_barrier_t` (and `csilk_barrier_t` underneath) must be heap-allocated — never stack-allocated when used across threads.
- Client recycling uses generation tags; verify `client->generation == gen && client->state == CSILK_CONN_CLOSING` before destroying.
- Server teardown order: drain active clients → drain timers → drain `wp->dispatch_async` queues → join workers → stop hot-reload → EBR grace period → run registered `_csilk_server_register_teardown()` LIFO callbacks (e.g., `mq`, `h3`, `session`, `ws_room`, `otlp_trace`) → destroy router → close event loop → free pools.

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
| `src/core/server/connection_{pool,state,timer,close,io}.c` | Core connection state machine; handles pool, lifecycle, generation tags, I/O |
| `src/core/server/server_lifecycle.c` → split into `server_rcu.c` + `server_driver.c` | Server startup, RCU router swap, driver injection, teardown hook registry |
| `src/core/server/server_shutdown.c` | Ordered teardown path |
| `src/middleware/jwt.c` | JWT HS256 middleware + validation policies |
| `src/middleware/waf.c` | Dynamic WAF filter (`csilk_waf_filter_*`) + pattern/IP rules |
| `src/modules/db/` | Database abstraction + SQLite/MySQL/PQ/Redis/Mongo drivers |
| `src/modules/vector/vector_hnsw.c` | HNSW skip-graph index with AVX2 SIMD distance kernels |
| `src/reflection/reflect.c` | Reflection marshal/unmarshal for struct↔JSON binding |
| `cmake/libcsilk.map` | Linker version script enforcing pure `csilk_*` symbol exports |
| `cmake/abi_baseline_symbols.txt` | Full-build exported symbol baseline (`SOVERSION 2`) |
| `scripts/check_abi_compatibility.sh` | Automated ABI export whitelist & baseline checker |
| `scripts/check_sync_primitives.sh` | Enforces `csilk/core/sync.h` usage (bans raw `pthread_*` in `src/`) |
| `scripts/check_version_sync.sh` | Verifies CMake version matches git tag; `--expected <ver>` for release validation |
| `scripts/csilkskel` | Interactive project scaffold generator (C API + CMake + test skeleton) |
| `scripts/tag-release.sh` | Bumps version across all 18 locations (src/, python/, cmake/, docs, ASCII diagrams) |
| `scripts/profile.sh` | CPU profiler harness (perf, flamegraph) |
| `scripts/run_benchmarks.sh` | Benchmark runner; compares against prior results via `compare_benchmarks.py` |
| `scripts/generate-sdk.py` | TypeScript/Python SDK generator from OpenAPI spec (`--url` or `--file`, `--lang typescript|python|both`) |
| `scripts/setup_bench_tools.sh` | Benchmark toolchain installer |
| `fuzz/fuzz.dict` | libFuzzer dictionary for fuzz_test and fuzz_headers harnesses |
| `fuzz/corpus/` | Seed corpora for each fuzzer |
| `share/csilk/apm_ui.html` | Embedded single-page Web APM dashboard served at `/admin/apm` |
| `share/swagger-ui/` | Swagger UI assets served at `/swagger` |
| `docs/en/` | English documentation: getting-started, architecture, design, performance-tuning, troubleshooting, user-manual, contributing |
| `docs/zh-CN/` | Chinese mirror of the above |

## Runtime & Tooling Preferences

- **CMake** ≥ 3.24 recommended (`LINK_GROUP:RESCAN` support; 3.11+ fallback works for sequential linking); no other build backend is supported.
- **ccache** is auto-detected and used as compiler launcher if available.
- **Web Core system dependencies**: `libyaml-dev`, `libssl-dev` (≥ 1.1.1), `zlib1g-dev`, `libuv1-dev` (or `liburing` via FetchContent).
- **Optional module dependencies**: `libsqlite3-dev` (`CSILK_ENABLE_DB`), `libcurl4-openssl-dev` (≥ 7.80.0, `CSILK_ENABLE_{AI,VECTOR,MCP}`), plus optional `libpq-dev`, `libmysqlclient-dev`, `hiredis`, `libmongoc` per DB driver.
- **Auto-fetched** via FetchContent: `yyjson`, plus `liburing` (when `CSILK_USE_URING=ON`) and fallback `llhttp`/`nghttp2`/`ngtcp2`/`nghttp3`/`wamr` when not found on system.
- **Compiler**: Clang 19+ is default; GCC 13+ for coverage jobs. Apple Clang and MSVC are rejected.
- **Shared libs**: `.so` on Linux (`SOVERSION 2`), `.dylib` on macOS. Always built alongside `.a` when `CSILK_BUILD_SHARED=ON`.
- **pkg-config**: `pkg-config --cflags --libs csilk` works after install (reflecting enabled modules).

## Testing & QA

### Test categories

| Category | Filter | Timeout | Notes |
|---|---|---|---|
| Unit (core, app, middleware, protocols, security, drivers, reflection, messaging) | `-E test_integration` | 10s–60s | Core property tests (crypto, bcrypt, UUID) use `/dev/urandom` salts — assert-hash tests are `#ifdef TEST_OOM`-guarded |
| Integration | `-R test_integration` | 30s | Live HTTP server fixture; also SSE, session, middleware-chain, OpenAPI, admin |
| Stress | `test_*_stress` | 30–60s | Multi-worker, RCU reader storm, hot-reload ramp |
| Package smoke | `-R test_cmake_package_target_split` | 180s | Installs build tree into temp prefix and compiles external `find_package(csilk)` consumer |
| Fuzz | `fuzz_*` | Manual (CI: 30–120s) | 6 Core harnesses (`fuzz_test`, `fuzz_url`, `fuzz_yaml`, `fuzz_headers`, `fuzz_h2`, `fuzz_websocket`) + 5 Full harnesses (`fuzz_raft_rpc`, `fuzz_mcp`, `fuzz_sql_rewrite`, `fuzz_wasm`, `fuzz_h3`); corpus in `fuzz/corpus/`; dict at `fuzz/fuzz.dict` |

### Coverage

Built with `-DCSILK_BUILD_FULL=ON -DUSE_COVERAGE=ON -DENABLE_OOM_TEST=OFF` under gcc (ASAN/TSAN incompatible). `.github/workflows/coverage.yml` enforces `--fail-under-line 72` via `gcovr`, excluding `flamegraph.c`, `redis_storage.c`, `workflow_debug.c`, `uring_vector.c`.

### CI matrix (`.github/workflows/ci.yml`)

- **test**: ubuntu-24.04 + macos × Debug + Release (`-DCSILK_BUILD_FULL=ON`); ASAN on Linux Debug; Python bindings on Release/Linux Debug.
- **lint**: `check-format`, `check_version_sync.sh`, `check_sync_primitives.sh`, `check_abi_compatibility.sh` (SOVERSION 2), `check_mermaid.py`, `tidy`.
- **fuzz**: clang + ASAN + libFuzzer (`-DCSILK_BUILD_FULL=ON`); all 11 harnesses with corpus replay.
- **arm64**: cross-compile via `aarch64-linux-gnu-gcc` (`-DCSILK_BUILD_FULL=ON`).
- **io_uring**: libuv+`UV_USE_IO_URING=1` and native `CSILK_USE_URING=ON` (`-DCSILK_BUILD_FULL=ON`).
- **tsan**: separate TSAN job with `halt_on_error=1` (`-DCSILK_BUILD_FULL=ON`).
- **examples**: builds examples and smoke-tests `example_server` and `example_app`.
- **db_integration**: tests SQLite, PostgreSQL, MySQL, Redis, MongoDB drivers against live service containers.
- **core-minimal**: builds `-DCSILK_BUILD_FULL=OFF -DCSILK_BUILD_SHARED=ON`, asserts zero SQLite/CURL in `ldd build_core/libcsilk.so`, runs Core unit tests.
- **coverage**: gcc + gcovr job (`.github/workflows/coverage.yml`).
- **release**: version bump + tag + PyPI wheel + GitHub release.

## Commit Convention

```
type(scope): 🎯 subject
```

Types with emojis: `feat ✨`, `fix 🐛`, `docs 📝`, `style 🎨`, `refactor ♻️`, `test ✅`, `build 📦`, `ci 👷`, `chore 🧹`. Emoji goes **after** the colon, one per commit, subject lowercase imperative mood.

## Versioning

Version is pinned in `cmake/options.cmake` as a single source of truth (`CSILK_VERSION_MAJOR`/`MINOR`/`PATCH`). Bump via `scripts/tag-release.sh` which updates all 18 version locations across `src/`, `python/`, `cmake/`, docs, and ASCII diagrams. Sync check: `./scripts/check_version_sync.sh` (or `--expected <ver>` for release validation).
