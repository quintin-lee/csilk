# 生命周期钩子系统使用指南

> **Version**: 0.5.0-dev | **Last updated**: 2026-06-29

csilk 的钩子系统（Hook System）允许用户在服务器和请求生命周期的关键节点注入自定义逻辑，无需修改框架代码。支持 6 种钩子类型，覆盖从服务器启动到请求结束的完整链路。

---

## 1. 钩子类型

```c
typedef enum {
    CSILK_HOOK_SERVER_START,   // 服务器事件循环启动前
    CSILK_HOOK_SERVER_STOP,    // 服务器关闭时
    CSILK_HOOK_CONN_OPEN,      // 新 TCP 连接建立时
    CSILK_HOOK_CONN_CLOSE,     // TCP 连接关闭时
    CSILK_HOOK_REQUEST_BEGIN,  // HTTP 请求解析完成时
    CSILK_HOOK_REQUEST_END,    // HTTP 响应发送完成后
    CSILK_HOOK_COUNT           // 哨兵值，非有效钩子类型
} csilk_hook_type_t;
```

| 钩子类型 | 触发时机 | 回调签名 |
|:---------|:---------|:---------|
| `CSILK_HOOK_SERVER_START` | 事件循环启动前 | `void(csilk_server_t*)` |
| `CSILK_HOOK_SERVER_STOP` | 服务器关闭时 | `void(csilk_server_t*)` |
| `CSILK_HOOK_CONN_OPEN` | TCP 连接建立 | `void(csilk_ctx_t*)` |
| `CSILK_HOOK_CONN_CLOSE` | TCP 连接关闭 | `void(csilk_ctx_t*)` |
| `CSILK_HOOK_REQUEST_BEGIN` | HTTP 请求解析完成 | `void(csilk_ctx_t*)` |
| `CSILK_HOOK_REQUEST_END` | HTTP 响应发送完成 | `void(csilk_ctx_t*)` |

---

## 2. 注册钩子

使用 `csilk_server_add_hook` 注册回调，同一钩子类型可注册多个处理函数：

```c
#include "csilk/csilk.h"

void on_server_start(csilk_server_t* s) {
    CSILK_LOG_I("Server starting on port %d", csilk_server_get_port(s));
}

void on_server_stop(csilk_server_t* s) {
    CSILK_LOG_I("Server shutting down gracefully");
}

void on_conn_open(csilk_ctx_t* c) {
    const char* ip = csilk_get_client_ip(c);
    CSILK_LOG_I("New connection from %s", ip);
}

void on_conn_close(csilk_ctx_t* c) {
    const char* ip = csilk_get_client_ip(c);
    CSILK_LOG_I("Connection closed: %s", ip);
}

void on_request_begin(csilk_ctx_t* c) {
    CSILK_LOG_I("Request: %s %s",
        csilk_get_method(c), csilk_get_path(c));
}

void on_request_end(csilk_ctx_t* c) {
    CSILK_LOG_I("Response sent: %d", csilk_get_status(c));
}

int main() {
    csilk_router_t* r = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r);

    // 注册钩子（同一类型可注册多个）
    csilk_server_add_hook(s, CSILK_HOOK_SERVER_START,  on_server_start);
    csilk_server_add_hook(s, CSILK_HOOK_SERVER_STOP,   on_server_stop);
    csilk_server_add_hook(s, CSILK_HOOK_CONN_OPEN,     on_conn_open);
    csilk_server_add_hook(s, CSILK_HOOK_CONN_CLOSE,    on_conn_close);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, on_request_begin);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_END,   on_request_end);

    csilk_router_add(r, "GET", "/hello", ...);
    csilk_server_run(s, 8080);
    csilk_server_free(s);
    return 0;
}
```

---

## 3. 典型应用场景

### 3.1 连接级速率限制

在 `CONN_OPEN` 钩子中实现连接频率限制：

```c
#include <stdatomic.h>

static atomic_int g_conn_count = 0;
static const int MAX_CONNECTIONS = 10000;

void conn_limit_hook(csilk_ctx_t* c) {
    int current = atomic_fetch_add(&g_conn_count, 1) + 1;
    if (current > MAX_CONNECTIONS) {
        CSILK_LOG_W("Connection limit exceeded: %d", current);
        // 框架会在合适时机拒绝新请求
    }
}

void conn_close_hook(csilk_ctx_t* c) {
    atomic_fetch_sub(&g_conn_count, 1);
}
```

### 3.2 请求耗时统计

在 `REQUEST_BEGIN` 和 `REQUEST_END` 钩子中测量请求耗时：

```c
void req_begin_hook(csilk_ctx_t* c) {
    // 在上下文存储中记录开始时间
    uint64_t* start = (uint64_t*)csilk_arena_alloc(
        csilk_get_arena(c), sizeof(uint64_t));
    *start = uv_hrtime();
    csilk_set(c, "req_start", start);
}

void req_end_hook(csilk_ctx_t* c) {
    uint64_t* start = (uint64_t*)csilk_get(c, "req_start");
    if (start) {
        uint64_t elapsed_us = (uv_hrtime() - *start) / 1000;
        CSILK_LOG_I("Request completed in %lluµs", elapsed_us);
    }
}
```

### 3.3 服务器就绪通知

在 `SERVER_START` 钩子中执行初始化确认或外部系统注册：

```c
void on_ready(csilk_server_t* s) {
    // 向服务注册中心（如 Consul、etcd）注册本服务
    int port = csilk_server_get_port(s);
    service_registry_register("my-service", port);

    // 发送就绪信号
    CSILK_LOG_I("Service registered on port %d", port);
}
```

### 3.4 优雅关闭清理

在 `SERVER_STOP` 钩子中执行资源清理和反注册：

```c
void on_shutdown(csilk_server_t* s) {
    // 向服务注册中心反注册
    service_registry_deregister("my-service");

    // 关闭数据库连接池
    csilk_db_pool_free(global_db_pool);

    // 持久化未完成的任务
    save_pending_tasks();

    CSILK_LOG_I("Cleanup complete, shutting down");
}
```

### 3.5 请求审计日志

在 `REQUEST_END` 钩子中记录完整的访问审计信息：

```c
void audit_log_hook(csilk_ctx_t* c) {
    const char* method  = csilk_get_method(c);
    const char* path    = csilk_get_path(c);
    const char* ip      = csilk_get_client_ip(c);
    const char* ua      = csilk_get_header(c, "User-Agent");
    int status          = csilk_get_status(c);

    CSILK_LOG_I("[AUDIT] %s %s %d | %s | %s",
        method, path, status, ip, ua ? ua : "-");
}
```

---

## 4. 与中间件的对比

| 维度 | 钩子系统 | 中间件 |
|:-----|:---------|:-------|
| 作用域 | 服务器/连接/请求生命周期 | HTTP 请求处理链 |
| 触发时机 | 任何生命周期节点 | 仅在请求处理链中 |
| 访问 Context | CONN/REQUEST 级别有 | 始终有 |
| 可中止链 | 否（仅观察） | 是（`csilk_abort`） |
| 典型用途 | 监控、审计、初始化、清理 | 认证、授权、修改请求/响应 |
| 多个处理器 | ✅ 同一钩子支持多个 | ✅ 链式执行 |

> **经验法则**：如果你想"观察"而不"干预"，用钩子；如果你想"修改"或"拦截"，用中间件。

---

## 5. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **MUST NOT** 在钩子中阻塞 | 所有钩子调用运行在事件循环线程上，阻塞会暂停所有 I/O |
| **SHOULD** 使用 `CSILK_LOG_*` 而非 `printf` | 日志钩子应使用框架的统一日志系统 |
| **SHOULD** 钩子函数保持轻量 | 耗时操作应调度到 libuv 线程池（`uv_queue_work`） |
| **MAY** 使用 `csilk_set` 在钩子间传递数据 | REQUEST_BEGIN 中存储，REQUEST_END 中读取 |
| **MAY** 钩子和中间件共存 | 例如钩子做统计埋点，中间件做认证授权 |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — 钩子系统](../../docs/module-design/hooks.md) | 钩子系统内部实现与设计决策 |
| [性能调优指南](../performance-tuning.md) | 钩子使用对性能的影响分析 |
