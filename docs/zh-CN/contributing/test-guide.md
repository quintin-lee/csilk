# 测试指南

本文档描述如何编写和运行 csilk 测试，包括测试框架使用、单元测试规范和 CI 集成。

---

## 1. 测试框架概述

csilk 使用 **CMake Test Driver** 框架，所有测试通过 `ctest` 运行。

| 测试类型 | 命名规则 | 说明 |
|:-----|:------:|------|
| 单元测试 | `test_*` | 测试单个函数/模块 |
| 集成测试 | `test_integration_*` | 测试模块间交互 |
| Fuzz 测试 | `fuzz_*` | 模糊测试 HTTP 解析器 |
| OOM 测试 | `test_oom*` | 内存压力测试 |

---

## 2. 编写单元测试

### 2.1 基础测试模板

```c
// tests/core/test_my_module.c
#include "csilk/csilk.h"
#include <assert.h>
#include <string.h>

void test_basic_function(void) {
    // Arrange
    int expected = 42;

    // Act
    int actual = my_function();

    // Assert
    assert(actual == expected);
    printf("✓ test_basic_function passed\n");
}

void test_with_context(void) {
     // Arrange: 创建 Router 并获取 Context
     csilk_router_t* router = csilk_router_new();
     csilk_server_t* server = csilk_server_new(router);
     
     // 创建客户端上下文（用于测试）
     // 注意：实际请求上下文通过服务端创建，此处仅作示例
     csilk_ctx_t* c = _csilk_ctx_new_for_test(router, server);

     // Act
     csilk_set(c, "test_key", "test_value");
     csilk_str_view_t value = csilk_get(c, "test_key");

     // Assert
     assert(value.len == 10);
     assert(memcmp(value.data, "test_value", 10) == 0);
     _csilk_ctx_free(c);

     printf("✓ test_with_context passed\n");
 }

int main() {
    test_basic_function();
    test_with_context();

    return 0;
}
```

### 2.2 测试宏使用

```c
// csilk_test.h（框架提供的测试宏）
#include "csilk/test.h"

// 断言宏
CSILK_TEST_ASSERT(x == 5, "Expected x to be 5");
CSILK_TEST_ASSERT_STR_EQUAL(expected, actual, "String mismatch");
CSILK_TEST_ASSERT_STR_CONTAINS(haystack, needle, "Needle not found in haystack");
CSILK_TEST_ASSERT_NONNULL(ptr, "Pointer is NULL");
```

### 2.3 测试覆盖模块

**当前测试覆盖统计**：

| 模块 | 测试文件数 | 覆盖率 | 状态 |
|:-----|:---------:|:------:|:----:|
| Router | 2 | 95%+ | ✅ |
| Server | 5 | 90%+ | ✅ |
| Context | 3 | 90%+ | ✅ |
| WebSocket | 3 | 85%+ | ✅ |
| Middleware | 12 | 90%+ | ✅ |
| Security | 3 | 85%+ | ✅ |
| AI Engine | 2 | 80%+ | ✅ |
| Workflow | 12 | 85%+ | ✅ |
| Database | 2 | 85%+ | ✅ |
| MQ | 6 | 90%+ | ✅ |
| HTTP/2 | 2 | 85%+ | ✅ |

---

## 3. 编写集成测试

### 3.1 完整请求处理流程

```c
// tests/integration/test_integration_request_flow.c
#include "csilk/csilk.h"
#include <assert.h>

void test_full_request_cycle(void) {
    // Arrange: 创建 Router 和 Server
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);

    // 注册处理器
    void handler(csilk_ctx_t* c) {
        csilk_json(c, 200, cJSON_CreateString("OK"));
    }
    csilk_handler_t handlers[] = {handler};
    csilk_router_add(router, "GET", "/test", handlers, 1);

    // Arrange: 启动服务器
    csilk_server_set_config(server, &(csilk_server_config_t){.port = 9090});
    csilk_server_run(server, 9090);

    // Act: 模拟客户端请求（使用 libcurl）
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:9090/test");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    CURLcode res = curl_easy_perform(curl);

    // Assert: 验证请求成功
    assert(res == CURLE_OK);

    // Cleanup
    curl_easy_cleanup(curl);
    csilk_server_stop(server);
    csilk_router_free(router);
    csilk_server_free(server);

    printf("✓ test_full_request_cycle passed\n");
}
```

---

## 4. Fuzz 测试（可选）

### 4.1 编写 Fuzz 目标

```c
// tests/fuzz_http_parser.c
#include "csilk/csilk.h"

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    // 初始化全局状态
    return 0;
}

void LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // 复制到可变缓冲区
    char* buf = malloc(size + 1);
    memcpy(buf, data, size);
    buf[size] = '\0';

    // 解析 HTTP
    llhttp_settings_t settings;
    llhttp_t parser;
    llhttp_init(&parser, HTTP_BOTH, &settings);

    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_value = on_header_value;
    settings.on_body = on_body;

    llhttp_execute(&parser, buf, size);

    free(buf);
}
```

### 4.2 运行 Fuzz 测试

```bash
# 编译 Fuzz 目标
cmake .. -DUSE_FUZZER=ON
make -C build fuzz_test

# 运行 Fuzz 测试（10 秒）
cd build && ./fuzz_test -max_total_time=10

# 运行 Fuzz 测试（无限，直到崩溃）
cd build && ./fuzz_test
```

---

## 5. OOM 测试（内存压力测试）

### 5.1 OOM 测试框架

```c
// tests/core/test_oom.c
#include "csilk/csilk.h"
#include <assert.h>

void test_oom_scenario(void) {
    // 模拟 OOM 场景
    csilk_arena_t* arena = csilk_arena_new(8);  // 非常小的 Arena

    // 分配多个大块，触发溢出
    void* ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = csilk_arena_alloc(arena, 1024);  // 每块 1KB
        assert(ptrs[i] != NULL);
    }

    // 应该在某个点失败
    void* last = csilk_arena_alloc(arena, 1024);
    assert(last == NULL);  // OOM 触发

    // 清理
    csilk_arena_free(arena);
    printf("✓ test_oom_scenario passed\n");
}
```

---

## 6. 并发测试

### 6.1 多线程测试

```c
// tests/middleware/test_concurrent.c
#include "csilk/csilk.h"
#include <pthread.h>

void* thread_func(void* arg) {
    csilk_ctx_t* c = (csilk_ctx_t*)arg;
    // 并发访问 Context
    csilk_set(c, "thread_key", "thread_value");
    csilk_str_view_t val = csilk_get(c, "thread_key");
    return NULL;
}

void test_concurrent_access(void) {
    const int num_threads = 16;

    pthread_t threads[num_threads];
    
    // 创建服务端上下文用于测试（实际应用中由请求触发）
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    csilk_ctx_t* c = _csilk_ctx_new_for_test(router, server);

    // 启动多个线程并发访问
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, thread_func, c);
    }

    // 等待所有线程完成
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    _csilk_ctx_free(c);
    csilk_router_free(router);
    csilk_server_free(server);
    printf("✓ test_concurrent_access passed\n");
 }
```

---

## 7. CI 集成

### 7.1 GitHub Actions 测试矩阵

csilk 的 CI 包含三个测试矩阵：

1. **libuv 后端**（默认）— Linux + macOS，Debug 和 Release 模式
2. **io_uring 后端**（可选）— Linux only，专用 `io_uring` job
3. **Fuzz 测试** — clang-19 仅 Linux

```yaml
# .github/workflows/ci.yml
name: CI

jobs:
  test:
    strategy:
      matrix:
        os: [ubuntu-24.04, macos-14]
        build_type: [Debug, Release]

    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: sudo apt-get install -y cmake clang clang-tidy ccache libyaml-dev libssl-dev zlib1g-dev libcurl4-openssl-dev

      - name: Configure
        run: cmake -B build -S . -DCMAKE_BUILD_TYPE=${{ matrix.build_type }}

      - name: Build
        run: cmake --build build -j$(nproc)

      - name: Run tests
        run: ctest --test-dir build --output-on-failure

  build_uring:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: sudo apt-get install -y cmake gcc-13 libyaml-dev libssl-dev zlib1g-dev libcurl4-openssl-dev

      - name: Configure with io_uring
        run: cmake -B build_uring -S . -DCMAKE_BUILD_TYPE=Release -DCSILK_USE_URING=ON

      - name: Build
        run: cmake --build build_uring -j$(nproc)

      - name: Run tests
        run: ctest --test-dir build_uring --output-on-failure
```

### 7.2 测试通过条件

- [ ] 所有 122+ 个单元测试通过（libuv 后端 122 个；io_uring 后端 120 个）
- [ ] ASan 检查无泄漏（Debug 构建时）
- [ ] 测试覆盖率 ≥ 85%（Release 构建时）
- [ ] 多平台测试通过（Linux + macOS）

---

## 8. 测试最佳实践

| 实践 | 说明 |
|:-----|------|
| **MUST** 测试边界条件 | 空输入、最大值、NULL 指针 |
| **MUST NOT** 使用硬编码值 | 使用宏或常量定义 |
| **SHOULD** 测试异常路径 | 测试错误码和错误处理 |
| **SHOULD** 使用 Mock 对象 | 避免真实依赖（数据库、网络） |
| **SHOULD** 测试并发场景 | 多线程/多进程测试 |
| **SHOULD** 保持测试独立 | 每个测试独立运行，不依赖其他测试 |

---

## 9. 添加新测试

### 9.1 创建测试文件

```bash
# 创建测试文件
touch tests/core/test_new_feature.c

# 在 CMakeLists.txt 中添加
add_executable(test_new_feature tests/core/test_new_feature.c)
target_link_libraries(test_new_feature csilk pthread)
add_test(NAME test_new_feature COMMAND test_new_feature)
```

### 9.2 测试文件命名规范

- 单元测试：`test_<module>.c`
- 集成测试：`test_integration_<feature>.c`
- Fuzz 测试：`fuzz_<target>.c`
- OOM 测试：`test_oom_<scenario>.c`

### 9.3 测试 Checklist

- [ ] 测试文件创建在 `tests/` 目录
- [ ] 测试通过 `ctest` 运行
- [ ] 添加到 `cmake/tests.cmake`
- [ ] 包含 `#include "csilk/csilk.h"`
- [ ] 包含必要的 `<assert.h>` 或 `CSILK_TEST_ASSERT`
- [ ] 包含 `main()` 函数
- [ ] 每个测试以 `✓` 前缀输出
- [ ] 清理资源（`csilk_router_free`, `csilk_server_free`, `_csilk_ctx_free` 等）

---

## 10. 获取帮助

**测试问题**？提交 Issue 时包含：

```markdown
## 测试失败

### 测试名称
test_my_module

### 错误信息
```
[完整错误堆栈]
```

### 环境
```bash
uname -a
gcc --version
cmake --version
```

### 复现步骤
1. [步骤 1]
2. [步骤 2]

### 预期行为
[期望结果]

### 实际行为
[错误输出]
```