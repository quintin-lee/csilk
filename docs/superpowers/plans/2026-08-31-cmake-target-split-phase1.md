# CMake Target 拆分第一阶段 Implementation Plan

> **状态：已实现并完成验证（2026-08-31）**
>
> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不移动源码、不改变公共头文件路径和 API 的前提下，将构建系统拆分为职责清晰的 CMake targets，并保留现有 umbrella、共享库、安装导出和 Python 构建兼容性。

**Architecture:** 先在 `cmake/sources.cmake` 建立唯一 source ownership，再在 `cmake/targets.cmake` 生成静态和共享模块 targets。`csilk_core` 保留为兼容 target，实际 runtime 实现归入新 `csilk_runtime`；`csilk_db` 与 `csilk_vector` 使用独立 source list 和 target。`csilk_shared` 只聚合各模块的 shared targets，不再使用旧的 `${CSILK_SOURCES}` 重复编译全部实现。

**Tech Stack:** C23, CMake ≥ 3.11, libuv/io_uring, OpenSSL, yyjson, nghttp2, llhttp, zlib, curl, SQLite3, libyaml, CTest, pkg-config

---

## Scope and invariants

本计划只改构建系统与构建元数据。以下内容 MUST 保持不变：

- `src/` 下源码物理路径；
- `include/csilk/` 下现有公共头文件路径和 API；
- `csilk.h` 的公共聚合内容；
- Python setuptools 入口 `csilk._dummy_ext` 与 `csilk_shared` 构建目标；
- `CSILK_VERSION_MAJOR/MINOR/PATCH` 和版本同步文件；
- 默认 libuv backend 与显式 `CSILK_USE_URING=ON` backend 语义；
- `csilk::core`、`csilk::db` 及现有模块 aliases 的可用性。

第一阶段允许新增以下 public CMake targets/aliases：

```text
csilk_crypto       / csilk::crypto
csilk_runtime      / csilk::runtime
csilk_protocols    / csilk::protocols
csilk_middleware   / csilk::middleware
csilk_reflection   / csilk::reflection
csilk_app          / csilk::app
csilk_vector       / csilk::vector
```

---

## File map

| Action | File | Responsibility |
|---|---|---|
| Modify | `cmake/sources.cmake` | 建立 source ownership，消除 core/http/db 重复归属 |
| Modify | `cmake/targets.cmake` | 创建新静态/shared targets，维护兼容 aliases，修正 shared 聚合 |
| Modify | `cmake/install.cmake` | 安装/导出新增 targets，扩展 pkg-config 生成列表与变量 |
| Create | `cmake/pkgconfig/csilk-crypto.pc.in` | crypto 模块 pkg-config 元数据 |
| Create | `cmake/pkgconfig/csilk-runtime.pc.in` | runtime 模块 pkg-config 元数据 |
| Create | `cmake/pkgconfig/csilk-protocols.pc.in` | protocols 模块 pkg-config 元数据 |
| Create | `cmake/pkgconfig/csilk-middleware.pc.in` | middleware 模块 pkg-config 元数据 |
| Create | `cmake/pkgconfig/csilk-reflection.pc.in` | reflection 模块 pkg-config 元数据 |
| Create | `cmake/pkgconfig/csilk-app.pc.in` | app 模块 pkg-config 元数据 |
| Create | `cmake/pkgconfig/csilk-vector.pc.in` | vector 模块 pkg-config 元数据 |
| Modify | `cmake/pkgconfig/csilk-core.pc.in` | 让旧 core pc 继续兼容并指向 runtime closure |
| Modify | `cmake/pkgconfig/csilk-db.pc.in` | 明确 db 不再隐含 vector |
| Modify | `cmake/pkgconfig/csilk-http.pc.in` | 反映新的 runtime/http/protocol 依赖 |
| Modify | `cmake/pkgconfig/csilk.pc.in` | umbrella Requires 补充新模块 closure |
| Modify | `cmake/csilk-config.cmake.in` | 确认新增 target export 的消费路径和模块列表 |
| Create | `tests/cmake/test_target_split.cmake` | 验证目标存在、source ownership 不重复、兼容 aliases 存在 |
| Modify | `CMakeLists.txt` | 注册 target-structure CTest 或 configure-time smoke test（仅在现有测试机制需要时） |
| Modify | `docs/superpowers/specs/2026-08-31-cmake-target-split-phase1-design.md` | 只在实现发现与已批准设计存在必要差异时补充状态/决策记录 |

不创建或移动任何 `src/`、`include/`、`tests/` 生产/功能源文件；target split smoke test 是 CMake 脚本测试，不改变既有 C 测试目录。

---

## Target dependency contract

实现必须保持以下无环依赖关系：

```text
csilk_base
  ↑
csilk_crypto
  ↑
csilk_runtime ← csilk_json
  ↑
csilk_tls
  ↑
csilk_http
  ↑
csilk_http2
  ↑
csilk_protocols
  ↑
csilk_middleware
  ↑
csilk_reflection
  ↑
csilk_app
```

旁路模块：

```text
csilk_db       → csilk_runtime + csilk_json
csilk_vector   → csilk_runtime + csilk_json
csilk_ai       → csilk_runtime + csilk_json
csilk_mq       → csilk_runtime
csilk_wasm     → csilk_runtime
csilk_bypass   → csilk_runtime
csilk_workflow → csilk_runtime + csilk_json + csilk_ai + csilk_mq + csilk_wasm
```

约束：

- `csilk_runtime` MUST NOT link `csilk_http`, `csilk_app`, `csilk_middleware`, `csilk_workflow` or any database/AI target。
- `csilk_http` MUST NOT compile app、middleware、reflection、permission、WebSocket、Swagger/OpenAPI 或 MCP source。
- `csilk_db` MUST NOT compile `src/drivers/vector/*.c`。
- `csilk_shared` MUST NOT compile `${CSILK_SOURCES}` in addition to module shared targets；每个实现源在 shared build 中只能编译一次。
- `src/core/test_utils.c` MUST NOT 被加入任何生产 library target；若现有测试依赖它，必须通过测试 target 的 `target_sources()` 私有加入。
- `src/protocols/mcp/mcp_server.c` 对 workflow internal header 的既有依赖本阶段保持，不得通过让 runtime 依赖 workflow 来“修复”。

---

## Task 1: Normalize source ownership in `cmake/sources.cmake`

**Files:**

- Modify: `cmake/sources.cmake`

### Step 1: Record the current source ownership baseline

确认当前 source list 中已经存在的实际文件：

```text
src/crypto/crypto.c
src/crypto/cipher_dispatch.c
src/crypto/base64.c
src/crypto/sha1.c
src/crypto/uuid.c
src/crypto/bcrypt.c
src/drivers/cipher/openssl.c
src/core/primitives/url.c
src/drivers/vector/*.c
src/reflection/*.c
```

不得在 source manifest 中添加当前文件系统不存在的路径。

### Step 2: Replace the foundation list

将 `CSILK_BASE_SOURCES` 从当前三文件扩展为：

```cmake
set(CSILK_BASE_SOURCES
    src/core/primitives/arena.c
    src/core/primitives/bounded_buf.c
    src/core/primitives/kv_store.c
    src/core/primitives/header_map.c
    src/core/primitives/query.c
    src/core/primitives/url.c
)
```

### Step 3: Add crypto ownership

新增：

```cmake
set(CSILK_CRYPTO_SOURCES
    src/crypto/base64.c
    src/crypto/sha1.c
    src/crypto/cipher_dispatch.c
    src/crypto/uuid.c
    src/crypto/crypto.c
    src/crypto/bcrypt.c
    src/drivers/cipher/openssl.c
)
```

从 `CSILK_CORE_SOURCES` 删除上述所有 crypto/cipher source。保留 `src/core/cache/mvcc_cache.c`、config、diagnostics 和 backend primitive，直到 runtime target 接管它们。

### Step 4: Add runtime ownership

新增 `CSILK_RUNTIME_SOURCES`，内容必须覆盖：

```cmake
set(CSILK_RUNTIME_SOURCES
    src/core/ctx/context.c
    src/core/ctx/ctx_accessors.c
    src/core/ctx/ctx_async.c
    src/core/ctx/ctx_defer.c
    src/core/ctx/ctx_json.c

    src/core/config/config.c
    src/core/config/logger.c
    src/core/config/hooks.c
    src/core/config/hot_reload.c

    src/core/cache/mvcc_cache.c
    src/core/primitives/recovery.c
    src/core/primitives/response.c
    src/core/primitives/router.c
    src/core/primitives/router_simd.c
    src/core/primitives/router_trie.c

    src/core/server/connection_pool.c
    src/core/server/connection_state.c
    src/core/server/timer_lifetime.c
    src/core/server/connection_timer.c
    src/core/server/connection_close.c
    src/core/server/connection_io.c
    src/core/server/connection.c
    src/core/server/server_lifecycle.c
    src/core/server/server_driver.c
    src/core/server/server_rcu.c
    src/core/server/server_shutdown.c
    src/core/server/server_worker.c
)
```

从 `CSILK_HTTP_SOURCES` 删除这些文件。从 `CSILK_CORE_SOURCES` 删除 config/cache/backend 文件，避免同一源码同时进入两个 target。

### Step 5: Keep backend ownership explicit

新增：

```cmake
set(CSILK_RUNTIME_IO_SOURCES
    src/core/uring/uring_buf.c
    src/core/uring/uring_sqpoll.c
    src/core/uring/uring_vector.c
)
```

将 io_uring 条件追加改为追加到 `CSILK_RUNTIME_SOURCES`：

```cmake
if(CSILK_USE_URING)
    list(APPEND CSILK_RUNTIME_SOURCES
        src/core/uring/uring_thread_pool.c
        src/core/uring/uring_fs.c
        src/core/uring/uring_io.c
        src/core/uring/uring_loop.c
        src/core/uring/uring_handle.c
        src/core/uring/uring_tcp.c
        src/core/uring/uring_stream.c
        src/core/uring/uring_write.c
        src/core/uring/uring_close.c
        src/core/uring/uring_timer.c
        src/core/uring/uring_run.c
    )
endif()
```

将 `CSILK_RUNTIME_IO_SOURCES` 合并到 runtime source list：

```cmake
list(APPEND CSILK_RUNTIME_SOURCES ${CSILK_RUNTIME_IO_SOURCES})
```

### Step 6: Reduce HTTP ownership

把 `CSILK_HTTP_SOURCES` 收敛为：

```cmake
set(CSILK_HTTP_SOURCES
    src/core/http/http1_parse.c
    src/core/http/http1_serialize.c
    src/core/http/http1_write.c
    src/core/http/http1_pipeline.c
    src/core/http/http1_response.c
    src/core/http/http1_zerocopy.c
    src/core/http/swar_http.c
)
```

`src/core/http/tls.c` 继续只属于 `CSILK_TLS_SOURCES`。

### Step 7: Separate protocol ownership

保留现有 `CSILK_HTTP2_SOURCES`，但第一阶段仅把 HTTP/2/3 source 留在其中：

```cmake
set(CSILK_HTTP2_SOURCES
    src/core/http/h2_callbacks.c
    src/core/http/h2_session.c
    src/core/http/h2_response.c
    src/core/http/h2.c
    src/protocols/h3.c
)
```

新增：

```cmake
set(CSILK_PROTOCOLS_SOURCES
    src/protocols/websocket.c
    src/protocols/ws_room.c
    src/protocols/swagger.c
    src/protocols/swagger_serve.c
    src/protocols/openapi_gen.c
    src/protocols/mcp/mcp_jsonrpc.c
    src/protocols/mcp/mcp_server.c
    src/protocols/mcp/mcp_client.c
)
```

### Step 8: Separate middleware, reflection and app ownership

新增分组 source lists：

```cmake
set(CSILK_MIDDLEWARE_SECURITY_SOURCES
    src/middleware/auth.c
    src/middleware/csrf.c
    src/middleware/jwt.c
    src/middleware/waf.c
    src/middleware/xdp_waf.c
    src/middleware/security_headers.c
)

set(CSILK_MIDDLEWARE_TRAFFIC_SOURCES
    src/middleware/ratelimit.c
    src/middleware/sliding_ratelimit.c
    src/middleware/circuit_breaker.c
)

set(CSILK_MIDDLEWARE_OBSERVABILITY_SOURCES
    src/middleware/logger.c
    src/middleware/metrics.c
    src/middleware/request_id.c
    src/middleware/otlp_trace.c
    src/middleware/otlp_exporter.c
)

set(CSILK_MIDDLEWARE_STREAMING_SOURCES
    src/middleware/sse.c
)

set(CSILK_MIDDLEWARE_TRANSFORM_SOURCES
    src/middleware/cors.c
    src/middleware/gzip.c
    src/middleware/multipart.c
    src/middleware/session.c
    src/middleware/static.c
    src/middleware/validate.c
    src/middleware/grpc_gateway.c
)

set(CSILK_MIDDLEWARE_SOURCES
    ${CSILK_MIDDLEWARE_SECURITY_SOURCES}
    ${CSILK_MIDDLEWARE_TRAFFIC_SOURCES}
    ${CSILK_MIDDLEWARE_OBSERVABILITY_SOURCES}
    ${CSILK_MIDDLEWARE_STREAMING_SOURCES}
    ${CSILK_MIDDLEWARE_TRANSFORM_SOURCES}
)

set(CSILK_REFLECTION_SOURCES
    src/reflection/reflect.c
    src/reflection/reflect_marshal.c
    src/reflection/reflect_unmarshal.c
    src/reflection/reflect_free.c
)

set(CSILK_APP_SOURCES
    src/app/app.c
    src/app/app_routes.c
    src/app/group.c
    src/app/admin.c
)
```

从 `CSILK_HTTP_SOURCES` 删除 middleware、app、reflection、permission 和 protocol source。

### Step 9: Split database and vector ownership

将数据库 list 改为：

```cmake
set(CSILK_DB_SOURCES
    src/drivers/db/db.c
    src/drivers/db/sqlite.c
)
```

保持可选 driver source 通过现有条件追加到 `CSILK_DB_SOURCES`：

```cmake
# mysql.c, postgres.c, redis.c, redis_storage.c, mongodb.c
```

新增：

```cmake
set(CSILK_VECTOR_SOURCES
    src/drivers/vector/vector.c
    src/drivers/vector/vector_simd.c
    src/drivers/vector/vector_hnsw.c
    src/drivers/vector/qdrant.c
    src/drivers/vector/milvus.c
)
```

从 `CSILK_DB_SOURCES` 删除全部 `src/drivers/vector/*.c`。

### Step 10: Define the monolithic source closure once

将 `${CSILK_SOURCES}` 改为所有实现 source list 的唯一拼接：

```cmake
set(CSILK_SOURCES
    ${CSILK_BASE_SOURCES}
    ${CSILK_CRYPTO_SOURCES}
    ${CSILK_JSON_SOURCES}
    ${CSILK_RUNTIME_SOURCES}
    ${CSILK_TLS_SOURCES}
    ${CSILK_HTTP_SOURCES}
    ${CSILK_HTTP2_SOURCES}
    ${CSILK_PROTOCOLS_SOURCES}
    ${CSILK_MIDDLEWARE_SOURCES}
    ${CSILK_REFLECTION_SOURCES}
    ${CSILK_APP_SOURCES}
    ${CSILK_DB_SOURCES}
    ${CSILK_VECTOR_SOURCES}
    ${CSILK_AI_SOURCES}
    ${CSILK_MQ_SOURCES}
    ${CSILK_WORKFLOW_SOURCES}
    ${CSILK_WASM_SOURCES}
    ${CSILK_BYPASS_SOURCES}
)
```

在本任务完成后用文本检查确认每个实现路径只出现于一个 ownership list；`CSILK_SOURCES` 的汇总重复出现是允许的，但模块之间不得交叉重复。

### Step 11: Configure-time source ownership test

执行 source-list 静态检查，确保以下路径不会在两个独立 module list 中重复：

```text
src/core/ctx/context.c
src/core/server/connection.c
src/core/http/http1_parse.c
src/middleware/gzip.c
src/reflection/reflect.c
src/drivers/vector/vector.c
src/crypto/crypto.c
```

若检查失败，先修正 source manifest，再进入 target 修改。

---

## Task 2: Add static targets and compatibility aliases

**Files:**

- Modify: `cmake/targets.cmake`

### Step 1: Add `csilk_crypto`

在 `csilk_base`、`csilk_json` 之后创建：

```cmake
add_library(csilk_crypto STATIC ${CSILK_CRYPTO_SOURCES})
set_target_properties(csilk_crypto PROPERTIES OUTPUT_NAME "csilk-crypto")
csilk_target_setup(csilk_crypto PUBLIC STATIC)
target_link_libraries(csilk_crypto PUBLIC
    csilk_base
    OpenSSL::Crypto
    Threads::Threads
)
add_library(csilk::crypto ALIAS csilk_crypto)
```

### Step 2: Convert `csilk_core` into the compatibility target

将原来编译 `${CSILK_CORE_SOURCES}` 的 `csilk_core` 改为 runtime implementation target：

```cmake
add_library(csilk_runtime STATIC ${CSILK_RUNTIME_SOURCES})
set_target_properties(csilk_runtime PROPERTIES OUTPUT_NAME "csilk-runtime")
csilk_target_setup(csilk_runtime PUBLIC STATIC)
target_link_libraries(csilk_runtime PUBLIC
    csilk_base
    csilk_crypto
    csilk_json
    OpenSSL::Crypto
    Threads::Threads
)
```

保留原有 YAML 与 backend conditional linkage，目标对象改为 `csilk_runtime`：

```cmake
if(TARGET csilk::yaml)
  target_link_libraries(csilk_runtime PUBLIC csilk::yaml)
endif()
if(CSILK_USE_URING)
  target_link_libraries(csilk_runtime PUBLIC uring)
else()
  target_link_libraries(csilk_runtime PUBLIC csilk::libuv)
  target_compile_definitions(csilk_runtime PUBLIC CSILK_USE_LIBUV)
endif()
if(NOT APPLE AND NOT WIN32)
  target_link_libraries(csilk_runtime PUBLIC m)
endif()
add_library(csilk::runtime ALIAS csilk_runtime)

add_library(csilk_core INTERFACE)
target_link_libraries(csilk_core INTERFACE csilk_runtime)
add_library(csilk::core ALIAS csilk_core)
```

`csilk_core` compatibility target MUST NOT compile source. If downstream packaging requires a physical `libcsilk-core.a`, this is a follow-up decision and MUST NOT be emulated by duplicating runtime objects in the same phase.

### Step 3: Repoint existing runtime extensions

修改以下静态 target 的依赖：

```cmake
csilk_wasm   → csilk_runtime
csilk_bypass → csilk_runtime
csilk_tls    → csilk_runtime
csilk_mq     → csilk_runtime
```

`csilk_http2` 依赖：

```cmake
csilk_runtime
csilk_tls
nghttp2
```

### Step 4: Reduce `csilk_http`

将 `csilk_http` 的 link closure 改为：

```cmake
target_link_libraries(csilk_http PUBLIC
    csilk_runtime
    csilk_json
    csilk_tls
    csilk_http2
)
```

保留 llhttp、OpenSSL 和 zlib 的现有 linkage；不要再由 HTTP target 直接链接 `csilk_mq`，除非编译后的 HTTP/1 source 确实仍引用 MQ symbol。若存在该引用，保留依赖并在 plan execution notes 中记录具体调用点；不得为“理想分层”牺牲链接正确性。

### Step 5: Add `csilk_protocols`

```cmake
add_library(csilk_protocols STATIC ${CSILK_PROTOCOLS_SOURCES})
set_target_properties(csilk_protocols PROPERTIES OUTPUT_NAME "csilk-protocols")
csilk_target_setup(csilk_protocols PUBLIC STATIC)
target_link_libraries(csilk_protocols PUBLIC
    csilk_http
    csilk_http2
    csilk_runtime
    csilk_json
)
add_library(csilk::protocols ALIAS csilk_protocols)
```

如果 MCP source 在当前实现上需要 workflow symbols，允许在 `csilk_protocols` 上增加 `csilk_workflow` 的 PRIVATE/PUBLIC 依赖以通过链接，但 MUST 记录这是第一阶段的已知反向依赖；不得把 workflow 加入 runtime。

### Step 6: Add middleware, reflection and app targets

```cmake
add_library(csilk_middleware STATIC ${CSILK_MIDDLEWARE_SOURCES})
set_target_properties(csilk_middleware PROPERTIES OUTPUT_NAME "csilk-middleware")
csilk_target_setup(csilk_middleware PUBLIC STATIC)
target_link_libraries(csilk_middleware PUBLIC
    csilk_http
    csilk_runtime
    csilk_json
    csilk_tls
    ZLIB::ZLIB
    OpenSSL::Crypto
    Threads::Threads
)
add_library(csilk::middleware ALIAS csilk_middleware)

add_library(csilk_reflection STATIC ${CSILK_REFLECTION_SOURCES})
set_target_properties(csilk_reflection PROPERTIES OUTPUT_NAME "csilk-reflection")
csilk_target_setup(csilk_reflection PUBLIC STATIC)
target_link_libraries(csilk_reflection PUBLIC csilk_runtime csilk_json)
add_library(csilk::reflection ALIAS csilk_reflection)

add_library(csilk_app STATIC ${CSILK_APP_SOURCES})
set_target_properties(csilk_app PROPERTIES OUTPUT_NAME "csilk-app")
csilk_target_setup(csilk_app PUBLIC STATIC)
target_link_libraries(csilk_app PUBLIC
    csilk_http
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_db
    csilk_ai
)
add_library(csilk::app ALIAS csilk_app)
```

在 `csilk_app` 创建之前，确保 `csilk_db` 和 `csilk_ai` targets 已定义；如现有 target 定义顺序不满足，移动 target block 的位置，不移动 source 文件。

### Step 7: Split database and vector targets

保持 `csilk_db` 名称，source 改为 `${CSILK_DB_SOURCES}`，链接改为：

```cmake
add_library(csilk_db STATIC ${CSILK_DB_SOURCES})
set_target_properties(csilk_db PROPERTIES OUTPUT_NAME "csilk-db")
csilk_target_setup(csilk_db PUBLIC STATIC)
target_link_libraries(csilk_db PUBLIC
    csilk_runtime
    csilk_json
    SQLite3::SQLite3
    CURL::libcurl
)
add_library(csilk::db ALIAS csilk_db)
```

新增：

```cmake
add_library(csilk_vector STATIC ${CSILK_VECTOR_SOURCES})
set_target_properties(csilk_vector PROPERTIES OUTPUT_NAME "csilk-vector")
csilk_target_setup(csilk_vector PUBLIC STATIC)
target_link_libraries(csilk_vector PUBLIC
    csilk_runtime
    csilk_json
    CURL::libcurl
)
add_library(csilk::vector ALIAS csilk_vector)
```

保留可选 client target 的条件链接在 `csilk_db`；vector target 不链接 MySQL、PostgreSQL、Redis 或 MongoDB client。

### Step 8: Update static umbrella

将 `csilk` interface target 的 link list 改为包含新 targets：

```cmake
target_link_libraries(csilk INTERFACE
    csilk_app
    csilk_workflow
    csilk_ai
    csilk_db
    csilk_vector
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_http
    csilk_http2
    csilk_tls
    csilk_mq
    csilk_wasm
    csilk_bypass
    csilk_runtime
    csilk_crypto
    csilk_json
    csilk_base
)
```

`csilk_core` 可作为兼容 target 保留在 umbrella closure 中，但不得重复造成 runtime object 的编译或链接；由于它是 interface target，优先不再显式列入 umbrella。

---

## Task 3: Mirror the static split for shared targets

**Files:**

- Modify: `cmake/targets.cmake`

### Step 1: Create shared targets for every new module

在现有 shared block 中新增：

```text
csilk_crypto_shared
csilk_runtime_shared
csilk_protocols_shared
csilk_middleware_shared
csilk_reflection_shared
csilk_app_shared
csilk_vector_shared
```

每个 shared target MUST 复用对应的非导出 object library，而不是再次直接编译 source list：

```cmake
add_library(csilk_crypto_shared SHARED $<TARGET_OBJECTS:csilk_crypto_objects>)
add_library(csilk_runtime_shared SHARED $<TARGET_OBJECTS:csilk_runtime_objects>)
add_library(csilk_protocols_shared SHARED $<TARGET_OBJECTS:csilk_protocols_objects>)
add_library(csilk_middleware_shared SHARED $<TARGET_OBJECTS:csilk_middleware_objects>)
add_library(csilk_reflection_shared SHARED $<TARGET_OBJECTS:csilk_reflection_objects>)
add_library(csilk_app_shared SHARED $<TARGET_OBJECTS:csilk_app_objects>)
add_library(csilk_vector_shared SHARED $<TARGET_OBJECTS:csilk_vector_objects>)
```

每个 target MUST 设置对应的 `OUTPUT_NAME`：

```text
csilk-crypto, csilk-runtime, csilk-protocols, csilk-middleware,
csilk-reflection, csilk-app, csilk-vector
```

### Step 2: Mirror dependencies using shared providers

共享依赖 MUST 使用 shared provider，避免 shared target 依赖静态 target：

```text
crypto_shared  → base_shared
runtime_shared → base_shared + crypto_shared + json_shared
http_shared    → runtime_shared + json_shared + tls_shared + http2_shared
protocols_shared → http_shared + http2_shared + runtime_shared + json_shared
middleware_shared → http_shared + runtime_shared + json_shared + tls_shared
reflection_shared → runtime_shared + json_shared
app_shared → http_shared + protocols_shared + middleware_shared + reflection_shared + db_shared + ai_shared
vector_shared → runtime_shared + json_shared
```

底层 OpenSSL、zlib、curl、SQLite、nghttp2、llhttp、libuv/io_uring 依赖保持与静态 target 一致。

### Step 3: Replace `csilk_core_shared`

不再用 `${CSILK_CORE_SOURCES}` 创建 shared runtime implementation。将 `csilk_core_shared` 改为兼容 interface target：

```cmake
add_library(csilk_core_shared INTERFACE)
target_link_libraries(csilk_core_shared INTERFACE csilk_runtime_shared)
add_library(csilk::core_shared ALIAS csilk_core_shared)
```

如果安装导出不接受 interface target 与现有 package 逻辑的组合，保留 `csilk_core_shared` 的实现 target，但其 source 必须是 `${CSILK_RUNTIME_SOURCES}` 且 `OUTPUT_NAME` 为 `csilk-runtime` 的兼容实现不能与 `csilk_runtime_shared` 同时存在；优先采用 interface alias，避免重复符号。

### Step 4: Rebuild `csilk_shared` as a pure umbrella

删除：

```cmake
add_library(csilk_shared SHARED ${CSILK_SOURCES})
```

替换为一个 shared umbrella target，不直接编译 implementation sources：

```cmake
add_library(csilk_shared SHARED ${CSILK_UMBRELLA_SHARED_SOURCES})
```

其中 `CSILK_UMBRELLA_SHARED_SOURCES` 必须为空是不够的，因为 CMake 不接受无 source 的普通 shared library；因此实现时采用以下可链接方案之一：

- 使用一个仅包含导出/锚点符号的已有 umbrella translation unit，且该文件只编译一次；或
- 将 `csilk_shared` 改为由统一 object library 生成，并让各模块 shared targets 不再重复编译同一 source。

实现者 MUST 在执行时选择一个能通过 linker/ABI 验证的方案，并在 `cmake/targets.cmake` 中明确注释 source ownership。不得保留旧的“模块 shared + `${CSILK_SOURCES}` monolithic shared”双重编译方案。

> 这里是本计划唯一需要根据实际 linker 行为做实现选择的步骤；验收条件固定为：`csilk_shared` 成功构建、无 duplicate symbol、模块 shared target 成功构建、Python extension 成功链接。

### Step 5: Add shared aliases

为新增 shared targets 添加：

```cmake
add_library(csilk::crypto_shared ALIAS csilk_crypto_shared)
add_library(csilk::runtime_shared ALIAS csilk_runtime_shared)
add_library(csilk::protocols_shared ALIAS csilk_protocols_shared)
add_library(csilk::middleware_shared ALIAS csilk_middleware_shared)
add_library(csilk::reflection_shared ALIAS csilk_reflection_shared)
add_library(csilk::app_shared ALIAS csilk_app_shared)
add_library(csilk::vector_shared ALIAS csilk_vector_shared)
```

---

## Task 4: Update install/export and pkg-config metadata

**Files:**

- Modify: `cmake/install.cmake`
- Create: `cmake/pkgconfig/csilk-crypto.pc.in`
- Create: `cmake/pkgconfig/csilk-runtime.pc.in`
- Create: `cmake/pkgconfig/csilk-protocols.pc.in`
- Create: `cmake/pkgconfig/csilk-middleware.pc.in`
- Create: `cmake/pkgconfig/csilk-reflection.pc.in`
- Create: `cmake/pkgconfig/csilk-app.pc.in`
- Create: `cmake/pkgconfig/csilk-vector.pc.in`
- Modify: existing relevant `cmake/pkgconfig/*.pc.in`

### Step 1: Install new static targets

将以下 target 加入 `CSILK_INSTALL_TARGETS`：

```cmake
csilk_crypto
csilk_runtime
csilk_protocols
csilk_middleware
csilk_reflection
csilk_app
csilk_vector
```

`csilk_core` 若为 interface compatibility target，保留在 export 列表前先验证 CMake 是否允许当前 install(TARGETS ...) 组合；若不允许，从 install artifact list 移除但继续通过 alias/export compatibility 处理，并在 package smoke test 中验证 `csilk::core`。

### Step 2: Install new shared targets

将以下 target 加入 `CSILK_SHARED_INSTALL_TARGETS`：

```cmake
csilk_crypto_shared
csilk_runtime_shared
csilk_protocols_shared
csilk_middleware_shared
csilk_reflection_shared
csilk_app_shared
csilk_vector_shared
```

`csilk_core_shared` 若为 interface compatibility target，不重复安装 implementation artifact。

### Step 3: Define pkg-config variables

在 `cmake/install.cmake` 增加：

```cmake
set(CSILK_CRYPTO_PC_REQUIRES_PRIVATE "openssl")
set(CSILK_CRYPTO_PC_LIBS_PRIVATE "-lssl -lcrypto")

set(CSILK_RUNTIME_PC_REQUIRES_PRIVATE "csilk-base csilk-crypto csilk-json")
set(CSILK_RUNTIME_PC_LIBS_PRIVATE "-lcrypto -lyaml")

set(CSILK_PROTOCOLS_PC_REQUIRES "csilk-http csilk-http2")
set(CSILK_PROTOCOLS_PC_REQUIRES_PRIVATE "")
set(CSILK_PROTOCOLS_PC_LIBS_PRIVATE "-lnghttp2")

set(CSILK_MIDDLEWARE_PC_REQUIRES "csilk-http csilk-runtime")
set(CSILK_MIDDLEWARE_PC_REQUIRES_PRIVATE "zlib")
set(CSILK_MIDDLEWARE_PC_LIBS_PRIVATE "-lz -lssl -lcrypto")

set(CSILK_REFLECTION_PC_REQUIRES "csilk-runtime csilk-json")
set(CSILK_REFLECTION_PC_LIBS_PRIVATE "")

set(CSILK_APP_PC_REQUIRES "csilk-http csilk-protocols csilk-middleware csilk-reflection")
set(CSILK_APP_PC_REQUIRES_PRIVATE "")
set(CSILK_APP_PC_LIBS_PRIVATE "")

set(CSILK_VECTOR_PC_REQUIRES "csilk-runtime csilk-json")
set(CSILK_VECTOR_PC_REQUIRES_PRIVATE "libcurl")
set(CSILK_VECTOR_PC_LIBS_PRIVATE "-lcurl")
```

实际变量名必须与新 `.pc.in` 文件中的 `@...@` 完全匹配。对 optional DB client 的变量继续使用当前条件逻辑。

### Step 4: Add new `.pc.in` templates

每个模板至少包含：

```ini
prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${prefix}/@CMAKE_INSTALL_LIBDIR@
includedir=${prefix}/@CMAKE_INSTALL_INCLUDEDIR@

Name: csilk-<module>
Description: csilk <module> module
Version: @CSILK_VERSION@
URL: https://github.com/quintin-lee/csilk
Requires: @MODULE_REQUIRES@
Requires.private: @MODULE_REQUIRES_PRIVATE@
Libs: -L${libdir} -lcsilk-<module>
Libs.private: @MODULE_LIBS_PRIVATE@
Cflags: -I${includedir}
```

对没有 public `Requires` 的模块，模板应输出空值而不是引用不存在的变量。

### Step 5: Extend `CSILK_PC_FILES`

将新模块加入：

```cmake
set(CSILK_PC_FILES
    csilk-base
    csilk-crypto
    csilk-core
    csilk-runtime
    csilk-json
    csilk-wasm
    csilk-bypass
    csilk-tls
    csilk-mq
    csilk-http2
    csilk-http
    csilk-protocols
    csilk-middleware
    csilk-reflection
    csilk-db
    csilk-vector
    csilk-ai
    csilk-app
    csilk-workflow
    csilk
)
```

`csilk-core.pc` MUST remain installable as a compatibility metadata file; its `Libs`/`Requires` MUST point to the new runtime closure without声明不存在的 `libcsilk-core` artifact。

### Step 6: Update umbrella metadata

`csilk.pc.in` 与生成变量 MUST 反映 `csilk-app`、`csilk-vector` 和新 runtime closure。至少确保以下命令在安装前的 generated `.pc` 文件上能解析：

```bash
pkg-config --define-prefix --print-requires ./build/csilk.pc
pkg-config --define-prefix --cflags --libs ./build/csilk.pc
```

---

## Task 5: Add CMake target-structure smoke tests

**Files:**

- Create: `tests/cmake/test_target_split.cmake`
- Modify: `CMakeLists.txt` only if registration is required by current CTest setup

### Step 1: Validate target existence in configure context

测试脚本必须检查：

```cmake
foreach(target
    csilk_base
    csilk_crypto
    csilk_runtime
    csilk_json
    csilk_tls
    csilk_http
    csilk_http2
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_app
    csilk_db
    csilk_vector
    csilk_ai
    csilk_mq
    csilk_workflow
)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "missing target: ${target}")
  endif()
endforeach()

foreach(alias
    csilk::core
    csilk::crypto
    csilk::runtime
    csilk::protocols
    csilk::middleware
    csilk::reflection
    csilk::app
    csilk::db
    csilk::vector
)
  if(NOT TARGET ${alias})
    message(FATAL_ERROR "missing compatibility/public alias: ${alias}")
  endif()
endforeach()
```

### Step 2: Validate source ownership

测试脚本或 configure-time helper MUST 对关键 source path 做唯一归属检查，至少覆盖：

```text
src/crypto/crypto.c
src/core/server/connection.c
src/core/http/http1_parse.c
src/middleware/gzip.c
src/reflection/reflect.c
src/drivers/db/db.c
src/drivers/vector/vector.c
```

### Step 3: Validate shared variant separately

Smoke test MUST 在 `CSILK_BUILD_SHARED=ON` 配置下确认：

```text
csilk_crypto_shared
csilk_runtime_shared
csilk_protocols_shared
csilk_middleware_shared
csilk_reflection_shared
csilk_app_shared
csilk_vector_shared
csilk_shared
```

测试不应直接调用测试 executable；构建和 CTest 注册必须遵循仓库现有 CMake 约定。

---

## Task 6: Update installed package consumer smoke test

**Files:**

- Modify: `cmake/csilk-config.cmake.in`
- Create: `tests/cmake/consumer/CMakeLists.txt`
- Create: `tests/cmake/consumer/main.c`

### Step 1: Keep module export names stable

`cmake/csilk-config.cmake.in` MUST continue加载 `csilk-targets.cmake` and expose：

```cmake
csilk::core
csilk::runtime
csilk::crypto
csilk::db
csilk::vector
csilk::app
csilk::csilk
```

Do not replace public include directories or generated version header paths.

### Step 2: Compile a minimal consumer

`tests/cmake/consumer/main.c` 使用现有公共 header 和最小 API：

```c
#include <csilk/csilk.h>

int
main(void)
{
    return 0;
}
```

Consumer `CMakeLists.txt` MUST use `find_package(csilk CONFIG REQUIRED)` and link `csilk::csilk`; it MUST NOT include repository-private `src/` paths.

### Step 3: Run consumer build after install

使用独立临时 prefix 和独立 build directory，避免修改现有 build 目录：

```bash
cmake --install build --prefix build/_install_target_split
cmake -S tests/cmake/consumer \
      -B build/_target_split_consumer \
      -DCMAKE_PREFIX_PATH="$PWD/build/_install_target_split"
cmake --build build/_target_split_consumer
```

如果现有 `build` 未启用 shared targets，使用新 variant 目录，不重复配置旧 build：

```bash
cmake -S . -B build_target_split_shared \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCSILK_BUILD_SHARED=ON \
      -DCSILK_BUILD_TESTS=ON \
      -DCSILK_BUILD_EXAMPLES=OFF
```

---

## Task 7: Verify default and io_uring build variants

**Files:**

- No source files changed; only build directories and generated artifacts

### Step 1: Build default libuv variant

使用现有已配置的 `build`，不重新 configure：

```bash
cmake --build build --target csilk_base csilk_crypto csilk_runtime
cmake --build build --target csilk_tls csilk_http csilk_http2 csilk_protocols
cmake --build build --target csilk_middleware csilk_reflection csilk_app
cmake --build build --target csilk_db csilk_vector csilk_ai csilk_mq csilk_workflow
cmake --build build --target csilk
```

如果 targets 在现有 build 中不存在，创建新的 `build_target_split` variant：

```bash
cmake -S . -B build_target_split \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCSILK_BUILD_TESTS=ON \
      -DCSILK_BUILD_EXAMPLES=OFF
cmake --build build_target_split --target csilk
```

### Step 2: Build shared variant

```bash
cmake -S . -B build_target_split_shared \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCSILK_BUILD_SHARED=ON \
      -DCSILK_BUILD_TESTS=ON \
      -DCSILK_BUILD_EXAMPLES=OFF
cmake --build build_target_split_shared --target csilk_shared
```

Expected：所有新 shared targets 构建成功，无 duplicate symbol、undefined reference 或 source-not-found 错误。

### Step 3: Build io_uring variant

```bash
cmake -S . -B build_target_split_uring \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCSILK_USE_URING=ON \
      -DCSILK_BUILD_TESTS=ON \
      -DCSILK_BUILD_EXAMPLES=OFF
cmake --build build_target_split_uring --target csilk_runtime csilk_http csilk
```

### Step 4: Run CTest

```bash
ctest --test-dir build_target_split \
      -E test_integration \
      --timeout 30 \
      --output-on-failure
```

对 shared/uring variant 至少运行 target smoke test 和受影响的 core/http/vector 测试：

```bash
ctest --test-dir build_target_split_shared \
      -R "target_split|test_router|test_http|test_vector|test_crypto|test_app" \
      --timeout 30 \
      --output-on-failure

ctest --test-dir build_target_split_uring \
      -R "target_split|test_uring|test_server|test_router" \
      --timeout 30 \
      --output-on-failure
```

仓库若未注册某个正则匹配的 smoke test，先检查 `ctest -N`，不得直接执行测试二进制绕过 CTest。

---

## Task 8: Run formatting, package and Python verification

### Step 1: Format CMake-adjacent CMake/test files according to repository tooling

```bash
cmake --build build_target_split --target format
cmake --build build_target_split --target check-format
```

CMake 和 `.pc.in` 文件若不由 clang-format 管理，保持现有项目缩进，不手工改变无关格式。

### Step 2: Validate Mermaid and version sync

设计文档中的 Mermaid 图必须通过：

```bash
python3 scripts/check_mermaid.py .
./scripts/check_version_sync.sh
```

### Step 3: Build Python shared library

```bash
cmake --build build_target_split_shared --target csilk_shared
cmake --build build_target_split_shared --target csilk_shared
```

然后按仓库 Python 指南运行：

```bash
python3 python/tests/test_csilk.py
```

若 Python 测试默认加载的是其他 build 输出，设置其现有项目支持的 library path；不要修改 Python API 或硬编码新 target 名称。

### Step 4: Inspect generated install metadata

检查：

```bash
test -f build_target_split_shared/csilk-runtime.pc
test -f build_target_split_shared/csilk-vector.pc
grep -n "csilk-runtime\|csilk-vector\|csilk-crypto" \
    build_target_split_shared/csilk-targets.cmake \
    build_target_split_shared/csilk*.pc
```

确认 installed `csilk-targets.cmake` 不引用未安装的 target 或静态/shared 错配 target。

---

## Task 9: Final review and commit boundary

### Step 1: Review changed files

```bash
git diff -- cmake/sources.cmake cmake/targets.cmake cmake/install.cmake cmake/csilk-config.cmake.in cmake/pkgconfig tests/cmake
```

确认：

- 没有 `src/` 或 `include/` 文件被移动或修改；
- 没有版本文件被修改；
- 没有 `${CSILK_SOURCES}` 与模块 shared targets 的重复编译；
- `csilk::core` 与 `csilk::db` 兼容入口仍存在；
- vector API 的显式 target 已公开；
- optional DB 条件没有泄漏到 vector target；
- no raw `uv_*`/`pthread_*` code changes were introduced。

### Step 2: Run repository status check

```bash
git status --short
```

### Step 3: Commit only after user authorization

本计划不自动提交实现。获得明确授权后，提交应按仓库约定使用：

```text
build(cmake): 📦 split framework targets by responsibility
```

提交必须只包含本阶段 CMake、pkg-config、consumer smoke test 和相关文档变更，不包含已有无关工作树修改。

---

## Execution notes

- 使用每个模块一个非导出 object library 的方案；静态 archive、模块 shared library 与 `csilk_shared` 共享同一 compilation owner。
- `csilk_core` 和 `csilk_core_shared` 保留为指向 runtime 的 interface compatibility targets，不安装重复 implementation archive。
- `csilk_permission` 作为实际 source audit 中发现的独立模块补入 target、安装/export、pkg-config 与 package smoke contract。
- package smoke 根据 `CSILK_BUILD_SHARED` 条件构建和检查 shared targets，因此 static-only 与 shared 配置均可验证。
- MCP/workflow 的既有耦合、HTTP/2 与 HTTP/3 合并、middleware 聚合等仍属于第二阶段遗留项。
- Python wrapper 当前在 `get_bindings()` 中无条件声明测试辅助符号；由于 `test_utils.c` 已移出生产 shared ABI，Python 测试需要后续增加 test-support library 的显式加载路径。

## Acceptance checklist

- [x] `CSILK_BASE_SOURCES`、`CSILK_CRYPTO_SOURCES`、`CSILK_RUNTIME_SOURCES`、`CSILK_PROTOCOLS_SOURCES`、`CSILK_MIDDLEWARE_SOURCES`、`CSILK_REFLECTION_SOURCES`、`CSILK_APP_SOURCES`、`CSILK_DB_SOURCES`、`CSILK_VECTOR_SOURCES` ownership 明确。
- [x] 每个实现 source 只属于一个模块 source list。
- [x] `csilk_core` 不再重复编译 runtime 实现，`csilk::core` 仍可用。
- [x] `csilk_db` 不再包含 vector source，`csilk::vector` 可用。
- [x] 新静态 targets 和 aliases 成功 configure/build。
- [x] 新 shared targets 和 aliases 成功 configure/build。
- [x] `csilk_shared` 无 duplicate symbol 和 undefined reference。
- [x] default libuv 与 io_uring variant 均能构建。
- [x] install/export package consumer 成功编译。
- [x] 新旧 pkg-config 文件生成且依赖闭包有效。
- [x] CTest 非 integration 测试通过；受影响测试通过。
- [ ] Python shared-library 测试通过（当前 Python wrapper 仍无条件绑定仅测试用的 `csilk_test_ctx_new/free`；需后续让 Python 测试加载 `csilk_test_support`，本阶段仅确认 `csilk_shared` 可构建）。
- [x] `check-format`、Mermaid 校验和 version sync 通过。
