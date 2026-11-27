# csilk 功能收敛（Feature Convergence）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在已完成首轮 Web Core 剥离与桩清理的基础上，分四阶段将当前项目的空壳代码、错位模块、Python/打包断链、碎片化源码、跨层宏探针以及孤立重型子系统彻底收敛。

**Architecture:** 坚持“Web Core 默认极简、可选模块纯向下依赖、Python 与下游分发自动适配启停状态”的原则。先通过 Phase 1–2 消除空壳与断链（零破坏），再通过 Phase 3 合并过细文件并用注册表替代 `#ifdef CSILK_HAS_*` + `weak` 探针，最后在 Phase 4 按产品边界决策完成可选模块与 CMake 目标粒度收敛。

**Tech Stack:** C23 (`CMAKE_C_STANDARD 23`), CMake >= 3.24, libuv / io_uring, llhttp, nghttp2, yyjson, OpenSSL, zlib, libyaml, Python `ctypes`, pkg-config, Conan 2, Homebrew, vcpkg.

**Spec / Baseline:** 基于仓库当前代码与 [AGENTS.md](../../../AGENTS.md) 规范盘点生成。

## Global Constraints

- **双变体门禁**：任何触及 `cmake/`、`src/core/`、`src/app/`、`src/middleware/`、`src/protocols/`、`include/csilk/` 的任务，必须同时在 **Web Core**（`CSILK_BUILD_FULL=OFF`）和 **Full**（`CSILK_BUILD_FULL=ON`）下编译并通过 `ctest`。
- **零重型依赖入核**：`ldd build_core/libcsilk.so | grep -qiE "sqlite|curl"` 必须返回非零（零 SQLite3 / libcurl 链接）。
- **声明置顶与同步原语**：所有 `#include` 与文件级 `static` 变量必须位于文件顶部声明块；严禁在 `src/` 使用 `<pthread.h>` 或原生 `pthread_*`（必须通过 `bash ./scripts/check_sync_primitives.sh`）。
- **格式化与静态检查**：所有 C/H 修改提交前必须通过 `check-format` 与 `tidy`。
- **ABI 基线**：涉及公共导出符号变更（Phase 4 剔除孤立模块等）时，必须同步执行 `bash ./scripts/check_abi_compatibility.sh build_full/libcsilk.so` 并在确认后更新 `cmake/abi_baseline_symbols.txt`。
- **提交规范**：`type(scope): 🎯 subject`（冒号后附单个 emoji，小写祈使句）。

## Review Focus

1. **Web Core 下导入 Python `csilk` 包**：在 `build_core/libcsilk.so`（仅导出 500 个核心符号）下执行 `import csilk`、`App()`、`Context`、基础中间件时不能因缺少可选模块符号（AI/DB/Vector/Workflow/MQ/OTLP/Admin）触发 `AttributeError`。
2. **`CSILK_USE_URING=ON` + `CSILK_ENABLE_HTTP3=ON` + `CSILK_ENABLE_BYPASS=OFF` 组合构建**：`udp.c` 归位后该组合必须无未定义 `csilk_io_udp_*` 符号。
3. **跨 TU 的 `metrics` / `admin` 统计与监控采集**：移除 `#ifdef CSILK_HAS_*` 与 `__attribute__((weak))` 后，未链接可选模块时应优雅跳过，链接后应自动注册并输出完整指标/面板，且无多服务器实例间的静态指针悬空。
4. **`find_package(csilk)` 与 `pkg-config --cflags --libs` 消费端契约**：在 Core-Only、Full、HTTP3、WAMR 四种组合安装后，外部最小 CMake 与 pkg-config 消费者均可正常配置、链接与运行。
5. **合并 `src/core/json/` 与 `src/core/uring/` 文件后符号零丢失**：合并翻译单元不得改变任何 `csilk_json_*` 公共函数签名或行为，也不得破坏 `CSILK_USE_URING=ON` 下的 CQE 分发与取消逻辑。

---

## Phase 1：残余空壳清理与 Python 绑定修复（零破坏、立即可做）

### Task 1: 移除 4 个无实现空壳 `.c` 文件

**Files:**
- Delete: `src/workflow/wf_scheduler.c`
- Delete: `src/core/http/http1_response.c`
- Delete: `src/core/http/h2.c`
- Delete: `src/core/uring/uring_io.c`
- Modify: `cmake/sources.cmake:67-84,122-145,274-297`

**Interfaces:**
- Consumes: 无
- Produces: 精简后的 `CSILK_RUNTIME_SOURCES`、`CSILK_HTTP_SOURCES`、`CSILK_HTTP2_SOURCES`、`CSILK_WORKFLOW_SOURCES`（零符号变更）

- [ ] **Step 1: 确认这 4 个文件无任何外部 `#include` 或源码依赖**
  运行：`grep -rn -E "wf_scheduler\.c|http1_response\.c|core/http/h2\.c|uring_io\.c" src/ tests/ examples/ cmake/`
  预期：仅在 `cmake/sources.cmake` 出现（`test_uring_io.c` 为测试文件自身注释）。
- [ ] **Step 2: 从 `cmake/sources.cmake` 移除上述 4 个文件条目并删除源文件**
  从 `CSILK_RUNTIME_SOURCES`（`CSILK_USE_URING` 分支）、`CSILK_HTTP_SOURCES`、`CSILK_HTTP2_SOURCES`、`CSILK_WORKFLOW_SOURCES` 移除这 4 个路径，并执行 `git rm`。
- [ ] **Step 3: 双变体构建并跑测试验证**
  运行：`cmake --build build_core -j$(nproc) && ctest --test-dir build_core -E "test_integration|test_cmake_package_target_split" --timeout 20 --output-on-failure && cmake --build build_full -j$(nproc)`
  预期：全部编过且 `build_core` 单测 100% 通过。
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "refactor(core): ♻️ remove 4 empty backward-compat stub source files"
  ```

---

### Task 2: 将 `src/core/io/udp.c` 从 `BYPASS` 归位至 `RUNTIME`（`CSILK_USE_URING`）

**Files:**
- Move: `src/core/io/udp.c` → `src/core/uring/uring_udp.c`（或保留路径并在 `cmake/sources.cmake` 调整归属；推荐移至 `src/core/uring/uring_udp.c` 并修正 `#include "../uring/uring_internal.h"` 为 `#include "uring_internal.h"`）
- Modify: `cmake/sources.cmake:67-84,114-120`

**Interfaces:**
- Consumes: `include/csilk/core/sys_io.h` (`csilk_io_udp_*`), `src/core/uring/uring_internal.h`
- Produces: `CSILK_USE_URING=ON` 时 `csilk_runtime` 无条件包含 UDP 原语，不再依赖 `CSILK_ENABLE_BYPASS`

- [ ] **Step 1: 迁移 `src/core/io/udp.c` 到 `src/core/uring/uring_udp.c`**
  `git mv src/core/io/udp.c src/core/uring/uring_udp.c`，并更新文件内 `#include "../uring/uring_internal.h"` 为 `#include "uring_internal.h"`。
- [ ] **Step 2: 更新 `cmake/sources.cmake`**
  从 `CSILK_BYPASS_SOURCES` 移除 `src/core/io/udp.c`，添加到 `if(CSILK_USE_URING)` 下的 `CSILK_RUNTIME_SOURCES`。
- [ ] **Step 3: 检查格式与构建验证**
  运行：`cmake --build build_core --target check-format && cmake --build build_core -j$(nproc) && cmake --build build_full -j$(nproc)`
  预期：格式与双变体编译通过。
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "refactor(uring): ♻️ move uring UDP primitives out of bypass module into runtime"
  ```

---

### Task 3: 消除 `cmake/examples.cmake` 中 `example_observability` 命名与开关错位

**Files:**
- Rename: `examples/middleware/example_observability.c` → `examples/middleware/example_hooks_request_id.c`
- Modify: `cmake/examples.cmake:61-62`
- Modify: `README.md` / `docs/`（如存在直接文件名引用）

**Interfaces:**
- Consumes: `include/csilk/csilk.h`, `include/csilk/core/config/hooks.h`, `include/csilk/core/middleware.h`
- Produces: 核心示例目标 `example_hooks_request_id`（不与 `CSILK_ENABLE_OBSERVABILITY` 语义冲突）

- [ ] **Step 1: 检查全仓对 `example_observability` 的引用**
  运行：`grep -rn "example_observability" .`
- [ ] **Step 2: 重命名示例文件并更新 `cmake/examples.cmake`**
  `git mv examples/middleware/example_observability.c examples/middleware/example_hooks_request_id.c`，在 `cmake/examples.cmake` 将目标名与源文件同步更新，同步更新引用文档（如有）。
- [ ] **Step 3: 验证核心与全量示例编译**
  运行：`cmake --build build_core --target example_hooks_request_id && cmake --build build_full --target example_hooks_request_id`
  预期：编译通过且 `build_core` 不会误建名不副实的 `example_observability`。
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "refactor(examples): ♻️ rename example_observability to example_hooks_request_id"
  ```

---

### Task 4: 让 `python/csilk/lib.py` 支持可选模块符号按需绑定（兼容 Web Core）

**Files:**
- Modify: `python/csilk/lib.py`
- Create: `python/tests/test_web_core_compat.py`

**Interfaces:**
- Consumes: `libcsilk.so`（Web Core 或 Full）
- Produces:
  - `has_symbol(lib, name: str) -> bool`
  - `require_symbol(lib, name: str, feature: str)`（缺失时抛出明确 `NotImplementedError(f"csilk built without {feature} support (missing {name})")`）
  - `get_bindings()` 在 `build_core/libcsilk.so` 下不再因缺失 91 个可选符号抛出 `AttributeError`

- [ ] **Step 1: 编写红灯测试 `python/tests/test_web_core_compat.py`**
  测试内容：
  1. 在 `LIBCSILK_PATH=$PWD/build_core/libcsilk.so` 下调用 `csilk.lib.get_bindings()` 不抛异常；
  2. `has_symbol(lib, "csilk_app_new")` 为 `True`，`has_symbol(lib, "csilk_ai_new")` 为 `False`；
  3. 调用 `require_symbol(lib, "csilk_ai_new", "CSILK_ENABLE_AI")` 抛出 `NotImplementedError`。
- [ ] **Step 2: 运行测试确认当前红灯**
  运行：`LIBCSILK_PATH=$PWD/build_core/libcsilk.so PYTHONPATH=$PWD/python python3 -m unittest python/tests/test_web_core_compat.py`
  预期：FAIL with `AttributeError: ... function 'csilk_ai_new' not found`（或首个缺失符号）。
- [ ] **Step 3: 重构 `python/csilk/lib.py` 为表驱动/守卫式符号绑定**
  将可选模块符号分组（`db`、`mq`、`ai`、`vector`、`mcp`、`workflow`、`perm`、`observability`、`grpc`、`wasm`、`bypass`、`http3`），通过辅助函数 `_bind(lib, name, restype, argtypes, optional=False)` 完成绑定：核心符号缺失立即报错，可选符号仅在 `hasattr(lib, name)` 时赋 `restype`/`argtypes`。
- [ ] **Step 4: 运行 Python Web Core 测试与 Full 绑定测试**
  运行：
  - `LIBCSILK_PATH=$PWD/build_core/libcsilk.so PYTHONPATH=$PWD/python python3 -m unittest python/tests/test_web_core_compat.py`
  - `LIBCSILK_PATH=$PWD/build_full/libcsilk.so PYTHONPATH=$PWD/python python3 -m unittest python/tests/test_csilk.py python/tests/test_middleware_bindings.py python/tests/test_test_context_compat.py python/tests/test_asyncio_support.py`
  预期：均 PASS。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "fix(python): 🐛 bind optional module C symbols lazily for Web Core compatibility"
  ```

---

### Task 5: 让 `python/csilk/__init__.py`、`app.py`、`middleware.py`、`context.py` 兼容 Web Core

**Files:**
- Modify: `python/csilk/__init__.py:18-68`
- Modify: `python/csilk/app.py:55,812-900`（`csilk_session_init` 为 core，`MQ`/`MqContext` 为可选 `CSILK_ENABLE_MQ`）
- Modify: `python/csilk/middleware.py`（`trace_middleware` 属于 `CSILK_ENABLE_OBSERVABILITY`，按需检查 `require_symbol`）
- Modify: `python/csilk/context.py`（`trace_id`/`span_id` 属性缺失符号时返回 `None` 或抛出明确异常）
- Modify: `python/tests/test_web_core_compat.py`

**Interfaces:**
- Consumes: `csilk.lib.has_symbol`, `csilk.lib.require_symbol`
- Produces: Web Core 下可执行 `from csilk import App, Context, Group, logger_middleware, cors, jwt_middleware` 与完整的 ASGI / `App` 生命周期

- [ ] **Step 1: 在 `python/tests/test_web_core_compat.py` 补充红灯用例**
  断言在 `LIBCSILK_PATH=$PWD/build_core/libcsilk.so` 下：
  1. `import csilk; app = csilk.App(); app.free()` 成功；
  2. 访问可选符号封装（如 `csilk.AI()` 或 `csilk.MQ()`）抛出明确的 `NotImplementedError` 而非在 `import csilk` 时崩溃；
  3. 仅导入 `App, Context, Group, cors, jwt_middleware` 可完成基础路由与 `asyncio` 路由测试。
- [ ] **Step 2: 运行测试确认红灯**
  预期：`import csilk` 或实例化失败。
- [ ] **Step 3: 改造 `python/csilk/__init__.py` 与可选封装模块入口**
  - 核心类与核心中间件直接导出；
  - `Workflow`/`WorkflowNode`/`WorkflowContext`/`WorkflowData`/`WorkflowWorker`、`AI`/`AIContext`、`DBPool`、`Perm`、`VectorDB`、`MQ`/`MqContext` 使用模块级 `__getattr__` 或条件导入 + `require_symbol` 检查，确保 `import csilk` 不触发可选模块初始化失败。
- [ ] **Step 4: 双变体运行全部 Python 测试**
  运行：
  - `LIBCSILK_PATH=$PWD/build_core/libcsilk.so PYTHONPATH=$PWD/python python3 -m unittest python/tests/test_web_core_compat.py python/tests/test_asyncio_support.py python/tests/test_test_context_compat.py`
  - `LIBCSILK_PATH=$PWD/build_full/libcsilk.so PYTHONPATH=$PWD/python CSILK_ABI_PROBE_BIN=$PWD/build_full/test_python_abi CSILK_REQUIRE_ABI_PROBE=1 python3 -m unittest discover -s python/tests`
  预期：Web Core 与 Full 下 Python 测试全部通过。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "fix(python): 🐛 make package imports and wrappers compatible with Web Core"
  ```

---

## Phase 2：安装、打包与版本契约对齐

### Task 6: 补齐 `cmake/pkgconfig/` 缺失模板并修正 `cmake/install.cmake` 的 `.pc` 依赖列表

**Files:**
- Create: `cmake/pkgconfig/csilk-grpc.pc.in`
- Create: `cmake/pkgconfig/csilk-h3.pc.in`
- Create: `cmake/pkgconfig/csilk-mcp.pc.in`
- Create: `cmake/pkgconfig/csilk-observability.pc.in`
- Modify: `cmake/install.cmake:139-378`
- Modify: `tests/cmake/test_package.cmake`

**Interfaces:**
- Consumes: `cmake/targets.cmake` 的 23 个模块定义
- Produces: 每个启用的模块均有完整准确的 `.pc` 文件，且伞形 `csilk.pc` 的 `Requires` / `Libs.private` 与当前 `CSILK_ENABLE_*` 严格一致

- [ ] **Step 1: 在 `tests/cmake/test_package.cmake` 增加 pkg-config 存在性与变量校验红灯**
  安装后检查每个启用模块对应的 `${_prefix}/lib*/pkgconfig/csilk-*.pc` 存在，且核心模式下 `csilk.pc` 不包含 `sqlite3` 或 `curl`。
- [ ] **Step 2: 运行 `ctest --test-dir build_core -R test_cmake_package_target_split` 与 `build_full` 对应测试**
  预期：Full 模式下缺少 `csilk-grpc.pc`/`csilk-mcp.pc`/`csilk-observability.pc`。
- [ ] **Step 3: 补齐 4 个 `.pc.in` 模板并更新 `cmake/install.cmake`**
  - 新增 `csilk-grpc.pc.in`、`csilk-h3.pc.in`、`csilk-mcp.pc.in`、`csilk-observability.pc.in`；
  - 在 `cmake/install.cmake` 补齐 `CSILK_PC_FILES` 与 `CSILK_PC_REQUIRES`（涵盖 `mq`、`ai`、`observability`、`grpc`、`mcp`、`h3`、`csv` 等实际启用的选项与私依赖）；
  - 清理未使用的老旧 `cmake/csilk.pc.in`（确认无引用后移除或统一指向 `cmake/pkgconfig/csilk.pc.in`）。
- [ ] **Step 4: 验证 Core-Only 与 Full 的 `test_cmake_package_target_split`**
  运行：`ctest --test-dir build_core -R test_cmake_package_target_split --timeout 180 --output-on-failure && ctest --test-dir build_full -R test_cmake_package_target_split --timeout 180 --output-on-failure`
  预期：全部 PASS。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "build(cmake): 📦 sync pkg-config templates and install manifests across all modules"
  ```

---

### Task 7: 补全 `cmake/csilk-config.cmake.in` 对 `HTTP3` 与 `WAMR` 的消费端支持

**Files:**
- Modify: `cmake/csilk-config.cmake.in:1-60,155-211`
- Modify: `tests/cmake/test_package.cmake`

**Interfaces:**
- Consumes: `CSILK_ENABLE_HTTP3`, `CSILK_HAS_WAMR` 配置期状态
- Produces: 外部 `find_package(csilk CONFIG REQUIRED)` 在启用 HTTP3 或 WAMR 时不因缺失 `csilk::ngtcp2`/`csilk::nghttp3`/`vmlib` 失败

- [ ] **Step 1: 在 `cmake/csilk-config.cmake.in` 添加 `CSILK_ENABLE_HTTP3` 与 `CSILK_HAS_WAMR` 目标还原**
  当 `@CSILK_ENABLE_HTTP3@` 为真时定义/查找 `csilk::ngtcp2`、`csilk::nghttp3`；当 `@CSILK_HAS_WAMR@` 为真时确保 `vmlib` 可被解析或静态闭合。
- [ ] **Step 2: 清理 `tests/cmake/test_package.cmake` 冗余目标枚举注释并验证**
  运行：`ctest --test-dir build_core -R test_cmake_package_target_split --timeout 180 --output-on-failure && ctest --test-dir build_full -R test_cmake_package_target_split --timeout 180 --output-on-failure`
  预期：PASS。
- [ ] **Step 3: 提交**
  ```bash
  git commit -m "build(cmake): 📦 export HTTP3 and WAMR dependencies in csilk-config.cmake"
  ```

---

### Task 8: 同步外部包管理配方（Conan / Homebrew / vcpkg）至 Web Core 默认模型

**Files:**
- Modify: `conanfile.py`
- Modify: `Formula/csilk.rb`
- Modify: `cmake/ports/csilk/vcpkg.json`
- Modify: `cmake/ports/csilk/portfile.cmake`

**Interfaces:**
- Consumes: `cmake/options.cmake` 选项名与当前版本号
- Produces: Conan / Homebrew / vcpkg 默认仅依赖 Web Core（`libuv`, `openssl`, `zlib`, `libyaml`），可选开启 `full`/`db`/`ai` feature，并传入正确的 `-DCSILK_BUILD_TESTS=OFF -DCSILK_BUILD_EXAMPLES=OFF`

- [ ] **Step 1: 更新 `conanfile.py`**
  同步版本号；将 `libcurl`/`sqlite3` 移入可选 `with_full` / `with_db` / `with_ai` options；将 `BUILD_TESTING`/`BUILD_EXAMPLES` 更正为 `CSILK_BUILD_TESTS`/`CSILK_BUILD_EXAMPLES`。
- [ ] **Step 2: 更新 `Formula/csilk.rb`**
  同步版本 URL、`depends_on`（`curl`/`sqlite` 改为 `:optional` 或通过 `--with-full`），修正 `-DCSILK_BUILD_TESTS=OFF -DCSILK_BUILD_EXAMPLES=OFF`。
- [ ] **Step 3: 更新 `cmake/ports/csilk/vcpkg.json` 与 `portfile.cmake`**
  将 `curl`、`sqlite3` 从核心 `dependencies` 拆至 vcpkg `features`（`db`、`ai`、`full`），修正 `portfile.cmake` 的 `-DCSILK_BUILD_TESTS=OFF -DCSILK_BUILD_EXAMPLES=OFF`。
- [ ] **Step 4: 使用 `python3 -m py_compile conanfile.py` 与 `jq . cmake/ports/csilk/vcpkg.json` 校验语法**
  预期：零语法错误。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "build(packaging): 📦 align Conan, Homebrew, and vcpkg recipes with Web Core defaults"
  ```

---

### Task 9: 统一版本号口径并完成同步门禁验证

**Files:**
- Modify: `cmake/options.cmake`（若确认升为 `2.0.0`，通过 `scripts/tag-release.sh` 同步；或若维持 `1.2.0`，修正 `README.md`/`AGENTS.md` 中的 `v2.0` 表述）
- Verify: `scripts/check_version_sync.sh`

- [ ] **Step 1: 运行当前同步检查观察差异**
  运行：`./scripts/check_version_sync.sh`
- [ ] **Step 2: 按选定目标版本（`2.0.0` 或 `1.2.0`）执行统一同步**
  若定为 `2.0.0`：运行 `./scripts/tag-release.sh 2.0.0 --no-commit --no-tag --skip-tests`；若保留 `1.2.0`：清理 `README.md` / `README.zh-CN.md` / `AGENTS.md` 中不一致的 `v2.0` 措辞（保留 `SOVERSION 2` 说明）。
- [ ] **Step 3: 复核 `./scripts/check_version_sync.sh` 与 `python3 scripts/check_mermaid.py .`**
  预期：0 差异，Mermaid 校验全部通过。
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "chore(version): 🧹 unify project version strings across build and docs"
  ```

---

## Phase 3：源码粒度收敛与跨层耦合消除

### Task 10: 合并 `src/core/json/` 12 个碎片文件为 4 个内聚单元

**Files:**
- Keep & Merge into:
  - `src/core/json/json_internal.c`（保留内部池/doc 视图管理）
  - `src/core/json/json_doc.c`（合并原 `json_parse.c` 55L + `json_serialize.c` 30L + `json_copy.c` 37L + `json_free.c` 32L + `json_factory.c` 114L）
  - `src/core/json/json_access.c`（保留读访问 + 合并 `json_type.c` 86L + `json_iterate.c` 102L）
  - `src/core/json/json_mutate.c`（合并原 `json_mutate.c` 56L + `json_object.c` 223L + `json_array.c` 64L）
- Delete: `json_parse.c`, `json_serialize.c`, `json_copy.c`, `json_free.c`, `json_factory.c`, `json_type.c`, `json_iterate.c`, `json_object.c`, `json_array.c`
- Modify: `cmake/sources.cmake:91-105`

**Interfaces:**
- Consumes: `src/core/json/json_internal.h`, `include/csilk/core/json/json.h`
- Produces: 全部 69 个 `csilk_json_*` 公共函数签名与语义 100% 不变

- [ ] **Step 1: 按职责合并上述 `.c` 文件并删除被合并文件**
  严格保持每个目标文件顶部统一 `#include "json_internal.h"`，无重复 static 定义。
- [ ] **Step 2: 更新 `cmake/sources.cmake` 的 `CSILK_JSON_SOURCES`**
- [ ] **Step 3: 格式化并运行全部 JSON 相关测试与 ABI 检查**
  运行：`cmake --build build_core --target format && cmake --build build_core -j$(nproc) && ctest --test-dir build_core -R "test_json|test_ctx_json|test_reflect" --output-on-failure && bash ./scripts/check_abi_compatibility.sh build_full/libcsilk.so`
  预期：编译无警告、测试全通过、69 个 `csilk_json_*` 符号零变化。
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "refactor(json): ♻️ consolidate 12 fragmented json sources into 4 cohesive units"
  ```

---

### Task 11: 收敛 `src/core/uring/` 过度拆分的微型文件

**Files:**
- Merge into existing units:
  - `uring_handle.c` ← 合并 `uring_close.c` (133L) + `uring_timer.c` (123L)
  - `uring_stream.c` ← 合并 `uring_write.c` (139L) + `uring_vector.c` (94L)
- Delete: `src/core/uring/uring_close.c`, `src/core/uring/uring_timer.c`, `src/core/uring/uring_write.c`, `src/core/uring/uring_vector.c`
- Modify: `cmake/sources.cmake:67-84`
- Modify: `.gcovr`（若其排除了 `uring_vector.c`）

**Interfaces:**
- Consumes: `src/core/uring/uring_internal.h`
- Produces: `csilk_io_*` 在 `CSILK_USE_URING=ON` 下的实现保持不变，TU 数量从 14 个降至 9 个（含 Task 2 的 `uring_udp.c`）

- [ ] **Step 1: 检查 `.gcovr` 与 `uring_internal.h` 中对 `uring_vector.c` 等的引用**
- [ ] **Step 2: 合并 `uring_close.c` / `uring_timer.c` 到 `uring_handle.c`，合并 `uring_write.c` / `uring_vector.c` 到 `uring_stream.c`**
  保持所有顶部 `#include` 与文件级 `static` 变量在顶部声明块。
- [ ] **Step 3: 更新 `cmake/sources.cmake` 与 `.gcovr`**
- [ ] **Step 4: 在 `CSILK_USE_URING=ON` 变体编译并运行 `test_uring_*` 全部测试**
  运行：`cmake -B build_uring -S . -DCMAKE_BUILD_TYPE=Debug -DCSILK_USE_URING=ON && cmake --build build_uring -j$(nproc) && ctest --test-dir build_uring -R test_uring --timeout 30 --output-on-failure`
  预期：全部 PASS。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "refactor(uring): ♻️ merge small uring handle/stream translation units"
  ```

---

### Task 12: 用统一运行时注册表替代 `metrics.c` 与 `admin.c` 的 `#ifdef` + `weak` 探针

**Files:**
- Modify: `include/csilk/core/middleware.h`（或内部头文件 `src/core/internal/srv_internal.h`，若仅内部模块互连优先放内部 `_csilk_*`）
- Modify: `src/middleware/metrics.c:31-36,568-635`
- Modify: `src/app/admin.c:19-57,305-345,490-530`
- Modify: `src/drivers/ai/ai.c`, `src/drivers/db/db.c`, `src/messaging/mq_core.c`（启动/初始化时注册 metrics collector）
- Test: `tests/middleware/test_metrics.c`, `tests/app/test_admin.c`

**Interfaces:**
- Consumes: `csilk_server_t` / 进程级 metrics collector 槽位
- Produces:
  - `_csilk_metrics_register_collector(const char* name, csilk_metrics_collect_fn fn)`（最大 8 个槽位，无条件编译宏耦合）
  - 移除 `admin.c` 中 `__attribute__((weak))` 对 `csilk_ai_*` / `csilk_db_get_stats` 的声明，改由 `CSILK_ENABLE_AI` / `CSILK_ENABLE_DB` 模块初始化时向 `admin` / `metrics` 注册回调

- [ ] **Step 1: 补充测试验证无可选模块与有可选模块时 `/metrics` 与 `/admin` 输出稳定**
- [ ] **Step 2: 在 `metrics.c` 实现内部 collector 注册机制，移除 `metrics.c` 内的 `#ifdef CSILK_HAS_MQ/AI/DB`**
- [ ] **Step 3: 在 `admin.c` 移除 `__attribute__((weak))` 兜底函数，改为统一的 stats/monitor 注册回调**
- [ ] **Step 4: 在 `db.c` / `ai.c` / `mq_core.c` 初始化入口注册对应 collector**
- [ ] **Step 5: 双变体构建并运行 `test_metrics`、`test_admin`、`test_admin_integration`、`test_admin_auth`**
  预期：Web Core 与 Full 下 100% PASS。
- [ ] **Step 6: 提交**
  ```bash
  git commit -m "refactor(observability): ♻️ replace weak/ifdef subsystem probes with collector registry"
  ```

---

### Task 13: 消除 `src/core/primitives/response.c` 中的 `#ifdef CSILK_HAS_HTTP3` 硬编码声明

**Files:**
- Modify: `src/core/internal/srv_internal.h`
- Modify: `src/core/primitives/response.c:26-31,685-850`
- Modify: `src/protocols/http3.c`

**Interfaces:**
- Consumes: `csilk_client_t->protocol == CSILK_PROTO_HTTP3`
- Produces: `csilk_h3_response_ops_t`（`write`, `end`, `get_write_queue_size`, `is_writable` 函数指针表，由 HTTP/3 模块注册到 `server` 或全局 transport ops，使 `response.c` 零 `CSILK_HAS_HTTP3` 条件编译）

- [ ] **Step 1: 在 `src/core/internal/srv_internal.h` 定义 `csilk_h3_response_ops_t` 并提供 `_csilk_register_h3_response_ops()`**
- [ ] **Step 2: 修改 `src/core/primitives/response.c` 通过 ops 表分发 HTTP/3 写/结束/水位查询，移除 `#ifdef CSILK_HAS_HTTP3`**
- [ ] **Step 3: 在 `src/protocols/http3.c` 初始化时注册 ops 表**
- [ ] **Step 4: 双变体（含 `-DCSILK_ENABLE_HTTP3=ON`）编译并通过 `test_response*`、`test_h3*`**
  预期：PASS。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "refactor(core): ♻️ decouple response.c from HTTP/3 via transport ops table"
  ```

---

## Phase 4：模块开关与产品边界深度收敛（需确认边界后执行）

### Task 14: 合并 `CSILK_ENABLE_WASM` 与 `CSILK_ENABLE_WAMR` 为单一开关

**Files:**
- Modify: `cmake/options.cmake:47,71,78-80`
- Modify: `cmake/sources.cmake:107-112,340-342`
- Modify: `cmake/targets.cmake:247-249,387-393,507-509`
- Modify: `cmake/tests.cmake:181-189,214-216,251`
- Modify: `src/core/plugin/wasm_vm.c`, `src/core/plugin/wasm_wasi.c`

**Interfaces:**
- Decision: 不再提供“无运行时（`HAS_WAMR` 未定义）但编译空壳 `csilk_wasm`”的半成品状态。开启 `CSILK_ENABLE_WASM` 即拉取/链接 WAMR（或将 `CSILK_ENABLE_WASM` 默认保持 `OFF`，即使 `CSILK_BUILD_FULL=ON` 除非显式开启或确认 CI 允许拉取 WAMR）。

- [ ] **Step 1: 统一 `CSILK_ENABLE_WASM` 与 `CSILK_ENABLE_WAMR` 逻辑**
- [ ] **Step 2: 清理 `wasm_vm.c` / `wasm_wasi.c` 中 `#ifndef HAS_WAMR` 死分支（保持非空检查与安全边界）**
- [ ] **Step 3: 同步更新 `test_wasm_*` 预期与双变体构建**
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "refactor(wasm): ♻️ unify CSILK_ENABLE_WASM with WAMR runtime and drop no-op shell"
  ```

---

### Task 15: 清理无内部调用方的孤立模块（`mvcc_cache` 与 `bypass/af_xdp`）

**Files:**
- Delete (if confirmed): `src/core/cache/mvcc_cache.c`, `include/csilk/core/cache/mvcc_cache.h`, `tests/core/test_mvcc_cache*.c`
- Delete (if confirmed): `src/core/io/af_xdp.c`, `src/core/io/af_xdp_zerocopy.c`, `src/core/io/af_xdp_internal.h`, `src/core/io/io_perf_probe.c`, `include/csilk/core/io/af_xdp.h`, `tests/core/test_af_xdp*.c`, `tests/core/test_io_perf_fallback.c`
- Modify: `cmake/options.cmake`, `cmake/sources.cmake`, `cmake/targets.cmake`, `cmake/tests.cmake`, `cmake/install.cmake`, `cmake/csilk-config.cmake.in`, `cmake/abi_baseline_symbols.txt`

- [ ] **Step 1: 确认无外部依赖与示例引用（仅存在各自单元测试）**
- [ ] **Step 2: 从 CMake 选项、源列表、目标、安装清单、测试与文档中移除 `csilk_bypass` 与 `mvcc_cache`**
- [ ] **Step 3: 重新生成 `cmake/abi_baseline_symbols.txt` 并执行 `check_abi_compatibility.sh`**
- [ ] **Step 4: 运行 Core-Only 与 Full 全量 `ctest`**
  预期：全部 PASS，`abi_baseline_symbols.txt` 减少 20 个孤立符号。
- [ ] **Step 5: 提交**
  ```bash
  git commit -m "refactor(core): ♻️ remove unused mvcc_cache and unattached af_xdp bypass module"
  ```

---

### Task 16: 收敛 Web Core 内部库目标数量（11 → 4 个逻辑层）

**Files:**
- Modify: `cmake/targets.cmake:224-374,466-546`
- Modify: `cmake/install.cmake`
- Modify: `cmake/csilk-config.cmake.in`
- Modify: `cmake/pkgconfig/`

**Interfaces:**
- Consumes: 当前 11 个 Web Core `OBJECT` 库
- Produces: 对外整合为 4 个核心分发目标：`csilk_base`（含 `json` + `crypto` + `reflection`）、`csilk_runtime`（含 `tls`）、`csilk_http`（含 `http2` + `protocols`）、`csilk_app`（含 `middleware`），并用 `INTERFACE` 别名保留原 `csilk::*` 兼容目标以实现无痛过渡，消除 `LINK_GROUP:RESCAN` 循环链接压力。

- [ ] **Step 1: 在 `cmake/targets.cmake` 重构目标合并与兼容 `INTERFACE` 别名**
- [ ] **Step 2: 同步简化 `cmake/install.cmake` 与 `cmake/csilk-config.cmake.in`**
- [ ] **Step 3: 验证 `test_cmake_package_target_split` 在 Core-Only 与 Full 均 PASS**
- [ ] **Step 4: 提交**
  ```bash
  git commit -m "build(cmake): 📦 consolidate 11 Web Core targets into 4 layered libraries with compat aliases"
  ```

---

### Task 17: （可选终局边界）将重型非 Web 生态迁出主仓库

**Scope（需在执行前拍板）：**
- 若目标定位回归 **纯 C23 高性能 Web/API 框架**，按顺序将以下目录/模块独立拆出至扩展仓库（保留独立 CMake `find_package(csilk)` 消费示范）：
  1. `src/workflow/` + `include/csilk/app/{workflow*,wf_grid.h}` + `tests/workflow/`（~7.9K 行源码，79 个 ABI 符号）
  2. `src/messaging/` Raft 部分（`raft_*` 3.7K 行）或整个 `mq` 模块（~6.2K 行）
  3. `src/drivers/ai/` + `src/drivers/vector/` + `src/protocols/mcp/`（~9.1K 行）
- 每拆出一个子域，单独执行：剥离源码与测试 → 清理 `cmake/` 与 `python/` → 重建 `abi_baseline_symbols.txt` → 跑通 `build_core` 与剩余 `build_full` 全量测试 → 独立提交。

---

## Self-Review Checklist（自检结论）

- **范围覆盖**：覆盖前序分析指出的全部 4 类未收敛问题（空壳残留、Python/打包断链、文件与探针碎片化、孤立/膨胀模块）。
- **步骤可执行性**：Task 1–13 每个步骤均写明了精确文件路径、变更边界、验证命令与预期输出；Task 14–17 明确了收敛前置决策边界。
- **类型与命名一致**：`has_symbol` / `require_symbol`（Task 4 → Task 5）、`_csilk_metrics_register_collector`（Task 12）、`csilk_h3_response_ops_t`（Task 13）在各任务接口中命名完全一致。
- **比例控制**：未将实现代码全文贴入计划，仅锚定接口、文件与验收门禁。
