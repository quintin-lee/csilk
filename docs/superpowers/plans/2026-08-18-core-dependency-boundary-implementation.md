# csilk-core 依赖边界解耦与最小 Runtime 重构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `csilk-core` 从包含 40+ 源文件的庞大单体库，重构为真正最小化的 L1 基础运行时核心，剥离出独立的 `csilk_json`、`csilk_wasm`、`csilk_bypass` 模块，并将 Context & Router 划归 `csilk_http`，建立严格单向无环的依赖层次体系。

**Architecture:** 参考 [`docs/superpowers/specs/2026-08-18-core-dependency-boundary-redesign.md`](file:///home/quintin/Data/source/c_cpp/server-c/docs/superpowers/specs/2026-08-18-core-dependency-boundary-redesign.md)。

**Tech Stack:** C23, CMake 3.22+, libuv / io_uring, OpenSSL, pkg-config, ctest.

---

## 任务清单

### Task 1: 源码分类与 `cmake/sources.cmake` 模块化重组

**Files:**
- Modify: `cmake/sources.cmake`

- [ ] **Step 1: 拆分 `CSILK_CORE_SOURCES` 为纯粹 L1 最小通用运行时**
  - 保留: `arena.c`, `bounded_buf.c`, `kv_store.c`, `mvcc_cache.c`, `config.c`, `logger.c`, `hot_reload.c`, `hooks.c`, `flamegraph.c`, `crypto/*.c`, `cipher/openssl.c` (以及 `uring_*.c` 当开启 io_uring)。
  - 移除 Context、Router、JSON、WASM、AF_XDP/DPDK。

- [ ] **Step 2: 创建 `CSILK_JSON_SOURCES`、`CSILK_WASM_SOURCES` 与 `CSILK_BYPASS_SOURCES`**
  - `CSILK_JSON_SOURCES`: 全部 13 个 `src/core/json/json_*.c`。
  - `CSILK_WASM_SOURCES`: `src/core/plugin/wasm_plugin.c`, `wasm_vm.c`, `wasm_wasi.c`。
  - `CSILK_BYPASS_SOURCES`: `src/core/io/af_xdp*.c`, `dpdk_pmd.c`, `io_perf_probe.c`。

- [ ] **Step 3: 将 Context、Router、HeaderMap、Query 划归 `CSILK_HTTP_SOURCES`**
  - 追加 `context.c`, `ctx_accessors.c`, `ctx_defer.c`, `ctx_json.c`, `recovery.c`, `header_map.c`, `query.c`, `response.c`, `router.c`, `router_simd.c`, `router_trie.c` 至 `CSILK_HTTP_SOURCES`。

---

### Task 2: CMake Targets 拓扑与链接图更新

**Files:**
- Modify: `cmake/targets.cmake`

- [ ] **Step 1: 新增 `csilk_json` / `csilk_json_shared` 静态与动态库 Target**
  - 链接 `csilk_core` 与 `csilk::yyjson`。
  - 注册别名 `csilk::json` / `csilk::json_shared`。

- [ ] **Step 2: 新增 `csilk_wasm` / `csilk_wasm_shared` 与 `csilk_bypass` Target**
  - 链接 `csilk_core`。
  - 注册别名 `csilk::wasm` / `csilk::bypass`。

- [ ] **Step 3: 更新 `csilk_http`、`csilk_workflow` 与 `csilk` 依赖**
  - `csilk_http` 链接 `csilk_core`, `csilk_json`, `csilk_tls`, `csilk_http2`, `csilk_mq`, `ZLIB::ZLIB`, `csilk::llhttp`。
  - `csilk_workflow` 链接 `csilk_wasm`, `csilk_json`。
  - `csilk`（Umbrella）链接 `csilk_http`, `csilk_db`, `csilk_workflow`, `csilk_wasm`, `csilk_json`。

---

### Task 3: pkg-config 模板与 `cmake/install.cmake` 更新

**Files:**
- Create: `cmake/pkgconfig/csilk-json.pc.in`
- Create: `cmake/pkgconfig/csilk-wasm.pc.in`
- Modify: `cmake/pkgconfig/csilk-core.pc.in`
- Modify: `cmake/pkgconfig/csilk-http.pc.in`
- Modify: `cmake/pkgconfig/csilk.pc.in`
- Modify: `cmake/install.cmake`

- [ ] **Step 1: 创建 `csilk-json.pc.in` 与 `csilk-wasm.pc.in`**
  - `csilk-json.pc.in`: `Requires: csilk-core`, `Libs.private: -lyyjson`。
  - `csilk-wasm.pc.in`: `Requires: csilk-core`。

- [ ] **Step 2: 精简 `csilk-core.pc.in`**
  - 移除对 `yyjson` 的声明，仅保留 `Requires.private: libuv/liburing libcrypto yaml-0.1`。

- [ ] **Step 3: 更新 `cmake/install.cmake`**
  - 将 `csilk-json` 与 `csilk-wasm` 加入 `CSILK_PC_FILES` 循环并安装至 `${CMAKE_INSTALL_LIBDIR}/pkgconfig`。

---

### Task 4: Header 边界卫生与反向依赖解耦

**Files:**
- Modify: `include/csilk/core/internal.h`
- Modify: `src/core/ctx/ctx_json.c`

- [ ] **Step 1: 清理 `include/csilk/core/internal.h`**
  - 确保 `internal.h` 仅声明 L1 运行时原语。
  - 移除任何上层模块的冗余内部宏与声明。

- [ ] **Step 2: 保持 Context 与 JSON/Reflection 自然向下单向流**
  - `src/core/ctx/ctx_json.c` 归属 `csilk_http`，通过 `csilk::http -> csilk::json` 正常使用 `csilk_json_t` 与反射解组。

---

### Task 5: 全量构建、测试与独立下游验证

**Files:**
- Test: 全量 169 个单元与集成测试
- Test: 独立最小下游应用编译验证

- [ ] **Step 1: 本地编译与 CTest 回归**
  - `cmake --build build -j$(nproc)`
  - `ctest --test-dir build -E test_integration --output-on-failure` (100% 通过)
  - `cmake --build build_uring -j$(nproc)`
  - `ctest --test-dir build_uring -E test_integration --output-on-failure` (100% 通过)

- [ ] **Step 2: 验证独立最小 Core 消费**
  - 编写最小仅依赖 `csilk::core` 的独立 C 测试程序，验证使用 `pkg-config --static --libs csilk-core` 时零 HTTP / JSON / WASM 符号污染。

- [ ] **Step 3: 代码风格与格式检查**
  - `cmake --build build --target check-format`
  - `cmake --build build --target tidy`
  - `./scripts/check_version_sync.sh`
