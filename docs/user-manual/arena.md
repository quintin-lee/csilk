# Arena 分配器实用指南

> **Version**: 0.5.0-dev | **Last updated**: 2026-06-29

csilk 的 Arena 分配器是一个**线性碰撞分配器**（Bump Allocator），每次分配约 3 条 CPU 指令，重置开销 ≤ 5ns。在 csilk 中，每个 HTTP 请求绑定一个 Arena，请求结束时一次性释放所有内存——无需逐次 `free`。

---

## 1. 为什么使用 Arena

传统 `malloc` / `free` 的问题：

| 问题 | 影响 |
|:-----|:------|
| `malloc` 慢 | 数百纳秒到数微秒 |
| 内存碎片 | 高并发场景内存碎片严重 |
| 逐次 `free` | 容易遗漏导致泄漏 |
| 线程锁 | 多线程 `malloc` 存在锁竞争 |

Arena 的优势：

| 优势 | 说明 |
|:-----|:------|
| **~3 CPU 指令/次分配** | 仅需递增指针，比 `malloc` 快 100× |
| **≤5ns 重置** | 请求结束一次性释放，无逐次 `free` |
| **零碎片** | 线性偏移分配 => 无碎片化 |
| **无需锁** | 单线程（绑定到事件循环）无竞争 |
| **panic 安全** | `csilk_ctx_defer` 确保资源清理 |

---

## 2. 获取 Arena

```c
// 从请求上下文获取 Arena
csilk_arena_t* arena = csilk_get_arena(c);
```

每个请求的 Context 内嵌一个 Arena，请求处理完自动重置。

---

## 3. 基本分配

```c
void handler(csilk_ctx_t* c) {
    csilk_arena_t* arena = csilk_get_arena(c);

    // 分配一个 int
    int* p = (int*)csilk_arena_alloc(arena, sizeof(int));
    *p = 42;

    // 分配一个结构体
    User* u = (User*)csilk_arena_alloc(arena, sizeof(User));
    u->id = 1;
    u->name = csilk_arena_strdup(arena, "Alice");

    // 分配一个缓冲区
    char* buf = (char*)csilk_arena_alloc(arena, 256);
    snprintf(buf, 256, "Hello, %s", u->name);

    csilk_string(c, 200, buf);
    // 无需 free —— 请求结束时 Arena 自动重置
}
```

---

## 4. 与 Context API 结合

csilk 的 Context API 内部使用 Arena，无需手动调用 `csilk_arena_alloc`：

```c
void efficient_handler(csilk_ctx_t* c) {
    // csilk_set_header / csilk_string 等函数内部使用 Arena 复制字符串
    csilk_set_header(c, "X-Custom", "value");  // 自动 Arena 分配

    // 解析 JSON 时使用 Arena
    cJSON* json = csilk_get_json(c);  // 从 Arena 分配

    // 零拷贝：直接引用请求体缓冲区（不复制）
    size_t len;
    const char* body = csilk_get_body(c, &len);
    // body 指向 TCP 接收缓冲区 —— 零拷贝！
}
```

---

## 5. Arena 与字符串

```c
void string_example(csilk_ctx_t* c) {
    csilk_arena_t* arena = csilk_get_arena(c);

    // 复制字符串到 Arena（请求结束时自动释放）
    char* name = csilk_arena_strdup(arena, "Alice");
    char* email = csilk_arena_strdup(arena, "alice@example.com");

    // 格式化为 Arena 字符串
    char* msg = (char*)csilk_arena_alloc(arena, 128);
    int n = snprintf(msg, 128, "User: %s <%s>", name, email);

    csilk_string(c, 200, msg);
}
```

---

## 6. Arena 与 JSON

```c
void json_with_arena(csilk_ctx_t* c) {
    csilk_arena_t* arena = csilk_get_arena(c);

    // 构造 JSON 响应
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", 42);
    cJSON_AddStringToObject(json, "name", "Alice");

    // cJSON_Print 分配到 Arena
    char* str = cJSON_PrintUnformatted(json);
    csilk_string(c, 200, str);

    cJSON_Delete(json);
    // str 由 Arena 管理 —— 请求结束自动释放
}
```

---

## 7. 零拷贝技巧

`csilk_get_body` 返回的指针直接引用 llhttp 解析器的接收缓冲区——没有 `malloc`/`memcpy` 开销：

```c
void zero_copy_handler(csilk_ctx_t* c) {
    // ❌ 低效方式：先读 body，再复制到 JSON 解析器
    const char* body = strdup(csil_get_body_str(c));  // 不必要的复制
    cJSON* json = cJSON_Parse(body);
    free(body);

    // ✅ 高效方式：body 指针直接引用接收缓冲区
    size_t len;
    const char* raw = csilk_get_body(c, &len);
    cJSON* json = cJSON_ParseWithLength(raw, len);   // 零拷贝！

    // 使用 json...
    csilk_json(c, 200, json);
    cJSON_Delete(json);
}
```

---

## 8. Deferred Cleanup（延迟清理）

`csilk_ctx_defer` 注册回调，在 Arena 重置前自动执行——适用于需要手动释放的外部资源：

```c
void cleanup_file(void* arg) {
    close(*(int*)arg);
}

void file_handler(csilk_ctx_t* c) {
    int fd = open("/tmp/data", O_RDONLY);
    if (fd < 0) {
        csilk_json_error(c, 500, "Cannot open file");
        return;
    }

    // 注册延迟清理：请求结束时自动关闭 fd
    csilk_ctx_defer(c, cleanup_file, &fd);

    // 读取文件并响应
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    csilk_string(c, 200, buf);
}
```

---

## 9. 跨请求数据（堆分配）

Arena 仅适用于请求作用域。跨请求数据需使用堆 `malloc`：

```c
// 全局配置（堆分配，跨请求共享）
static Config* g_config = nullptr;

void load_config(void) {
    g_config = malloc(sizeof(Config));
    // ... 初始化
}

void handler(csilk_ctx_t* c) {
    // 请求内数据用 Arena
    char* tmp = csilk_arena_strdup(csil_get_arena(c), "temp");

    // 全局数据用堆
    csilk_string(c, 200, g_config->greeting);
}
```

---

## 10. 性能对比

| 操作 | Arena | malloc |
|:-----|:-----:|:------:|
| 一次分配 | **~3 CPU 指令** | ~100-500 ns |
| 释放 | **≤5 ns（整体重置）** | O(n) 遍历释放 |
| 碎片 | **零** | 易碎片化 |
| 线程安全 | 否（单线程使用） | 是（原子操作） |

在 csilk 的 HTTP 请求处理中，**所有字符串复制、头部分配、JSON 序列化**都优先使用 Arena：

- `csilk_string(c, 200, msg)` → msg 复制到 Arena
- `csilk_set_header(c, k, v)` → k/v 复制到 Arena
- `csilk_json(c, 200, json)` → JSON 字符串分配到 Arena

---

## 11. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **SHOULD** 请求内数据优先用 Arena | `csilk_arena_alloc` / `csilk_arena_strdup` 而非 `malloc` |
| **SHOULD** 使用 `csilk_get_body` 零拷贝 | 直接引用接收缓冲区，不复制 |
| **SHOULD** 使用 `cJSON_ParseWithLength` | 传入长度避免 `strlen` 额外扫描 |
| **MAY** 使用 `csilk_ctx_defer` 管理外部资源 | 文件描述符、数据库连接等 |
| **MUST NOT** 在 Arena 分配后 `free` | Arena 整体重置，单次 `free` 会导致崩溃 |
| **MUST NOT** 跨请求持有 Arena 指针 | Arena 数据在请求结束后无效 |
| **SHOULD** 将 `cJSON_Print` 结果放入 Arena | 分配后可直接响应，无需额外 `free` |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — Arena](../../docs/module-design/arena.md) | Arena 内部实现：Bump 指针、SIMD memcpy、形式化验证 |
| [架构白皮书 — 零拷贝](../architecture.md) | 零拷贝 HTTP 解析设计 |
| [性能调优指南](../performance-tuning.md) | Arena 调优与编译器优化 |
