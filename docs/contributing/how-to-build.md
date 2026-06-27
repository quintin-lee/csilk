# How to Build

本文档描述如何构建 csilk 框架，包括开发环境配置、编译选项说明和常见构建问题排查。

---

## 1. 环境要求

### 1.1 编译器要求

| 编译器 | 最低版本 | 推荐版本 | 说明 |
|:-----|:--------:|:--------:|------|
| GCC | 13.0+ | **13.2** | 完整 C23 支持 |
| Clang | 19.0+ | **19.1** | libFuzzer 集成 |
| Apple Clang | — | — | **不支持**（缺少 C23 constexpr） |
| MSVC | — | — | **不支持**（POSIX API 依赖） |

**验证编译器版本**：
```bash
gcc --version  # MUST 输出 gcc 13.0+
clang --version  # MUST 输出 clang 19.0+
```

### 1.2 系统依赖

**Ubuntu/Debian**：
```bash
sudo apt-get update
sudo apt-get install -y \
    cmake \
    gcc \
    libyaml-dev \
    zlib1g-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libpq-dev \
    libsqlite3-dev \
    clang-tidy \
    ccache
```

**macOS**：
```bash
brew install cmake llvm libyaml zlib openssl curl libpq sqlite3 ccache
```

---

## 2. 编译步骤

### 2.1 标准编译（Release 模式）

```bash
# 克隆仓库
git clone https://github.com/yourusername/csilk.git
cd csilk

# 创建构建目录
mkdir build && cd build

# 配置（Release 模式）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

# 验证构建
./example_server --help  # 应输出帮助信息
```

### 2.2 Debug 模式（启用 ASan）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DUSE_ASAN=ON
make -j$(nproc)
```

### 2.3 启用所有优化（LTO + PGO）

```bash
# Phase 1: Instrumented 构建
cmake -B build_pgo -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_FLAGS="-fprofile-generate"
make -C build_pgo -j$(nproc)

# Phase 2: 运行基准测试
./build_pgo/example_server &
curl -s http://localhost:8080/healthz

# Phase 3: 使用 Profile 重新编译
cmake -B build_pgo_final -S . -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-fprofile-use"
make -C build_pgo_final -j$(nproc)
```

### 2.4 静态库 vs 动态库

```bash
# 静态库（默认，~150KB）
cmake .. -DCMAKE_BUILD_TYPE=Release
# 输出: libcsilk.a

# 动态库（~50KB + 依赖）
cmake .. -DCMAKE_BUILD_TYPE=Release -DCSILK_BUILD_SHARED=ON
# 输出: libcsilk.so
```

---

## 3. 编译选项说明

| 选项 | 默认值 | 说明 | 推荐生产环境 |
|:-----|:------:|------|:------------:|
| `CMAKE_BUILD_TYPE` | Release | Debug / Release / RelWithDebInfo | Release |
| `CSILK_BUILD_SHARED` | OFF | 启用动态库 | Release 时启用 |
| `USE_ASAN` | OFF | 启用 AddressSanitizer | 调试时 |
| `USE_FUZZER` | OFF | 启用 libFuzzer | CI 测试 |
| `USE_COVERAGE` | OFF | 启用 gcov 覆盖率 | 测试时 |
| `ENABLE_OOM_TEST` | OFF | 启用 OOM 模拟测试 | 测试时 |
| `CSILK_USE_MYSQL` | OFF | 启用 MySQL 驱动 | 需要时 |
| `CSILK_USE_POSTGRES` | OFF | 启用 PostgreSQL 驱动 | 需要时 |
| `CSILK_USE_MONGODB` | OFF | 启用 MongoDB 驱动 | 需要时 |
| `DEBUG_ARENA` | OFF | Arena 调试模式（检测溢出） | 调试时 |

---

## 4. 热重载（仅开发模式）

```bash
# 使用 --hot-reload 标志
./example_server --hot-reload
```

**MUST NOT**: 在生产环境启用热重载（无锁保护）。**SHOULD**: 仅在开发阶段使用。

---

## 5. 常见构建错误

### 5.1 C23 编译错误

**错误**：
```
error: 'constexpr' specifier is incompatible with C standards before C23
```

**解决**：
```bash
# 升级 GCC/Clang
sudo apt-get install gcc-13 clang-19

# 强制使用 C23
cmake .. -DCMAKE_C_STANDARD=23 -DCMAKE_BUILD_TYPE=Release
```

### 5.2 缺少依赖

**错误**：
```
CMake Error: Could NOT find libyaml
CMake Error: Could NOT find OpenSSL
```

**解决**：
```bash
# Ubuntu/Debian
sudo apt-get install libyaml-dev libssl-dev zlib1g-dev

# macOS
brew install libyaml openssl zlib curl
```

### 5.3 编译缓存问题

**症状**：修改代码后未生效

**解决**：
```bash
# 清理构建缓存
rm -rf build

# 重新配置和编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make clean && make -j$(nproc)
```

### 5.4 并发编译错误

**错误**：
```
error: implicit declaration of function 'csilk_arena_alloc'
```

**解决**：
```bash
# 确保使用最新代码
git pull

# 清理并重新编译
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## 6. 验证构建

### 6.1 运行测试

```bash
# 运行所有测试
ctest --test-dir build --output-on-failure

# 运行特定测试
ctest --test-dir build -R test_router --output-on-failure

# 运行带 ASan 的测试
export ASAN_OPTIONS=detect_leaks=1:symbolize=1
ctest --test-dir build --output-on-failure
```

### 6.2 运行示例

```bash
# Low-level API
./build/example_server

# High-level App API
./build/example_app

# Admin Dashboard
./build/example_admin_dashboard
```

### 6.3 生成文档

```bash
# 生成 API 文档（需要 Doxygen）
make docs

# 打开文档
open docs/html/index.html  # macOS
xdg-open docs/html/index.html  # Linux
```

---

## 7. 性能基准构建

```bash
# 编译带调试信息的 Release 构建（用于 perf 分析）
cmake -B build_profile -S . \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_FLAGS="-fprofile-generate"

make -C build_profile -j$(nproc)

# 生成 profile 数据
./build_profile/example_server &
# ... 生成 traffic ...

# 生成火焰图
make profile
```

---

## 8. 构建 Checklist

- [ ] CMake 3.11+ 已安装
- [ ] GCC 13.0+ 或 Clang 19.0+
- [ ] 系统依赖（libyaml-dev, libssl-dev, zlib1g-dev, libcurl4-openssl-dev）
- [ ] 已配置 `CMAKE_BUILD_TYPE=Release`
- [ ] 已运行 `cmake ..`
- [ ] 已运行 `make -j$(nproc)`
- [ ] 测试通过（`ctest`）
- [ ] 示例可运行（`./example_server`）

---

## 9. 获取帮助

**构建问题**？提交 Issue 时包含：

```markdown
## 构建错误

### 错误信息
```
[完整错误堆栈]
```

### 环境信息
```bash
gcc --version
cmake --version
uname -a
```

### 复现步骤
1. [步骤 1]
2. [步骤 2]

### 预期行为
[期望结果]

### 实际行为
[错误输出]
```
