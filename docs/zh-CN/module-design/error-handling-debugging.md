# 错误处理与调试指南

> **Version**: 0.5.1 | **Last updated**: 2026-08-22

本文档深入解析 csilk 的错误处理机制、日志系统和调试工具。

---

## 1. 错误码体系

### 1.1 核心错误码

```c
typedef enum csilk_ret_e {
    CSILK_OK = 0,           // 成功
    CSILK_ERR = -1,         // 通用错误
    CSILK_ERR_MEMORY = -2,  // 内存分配失败
    CSILK_ERR_INVALID = -3, // 参数无效
    CSILK_ERR_NOT_FOUND = -4, // 未找到
    CSILK_ERR_BUSY = -5,    // 资源繁忙
    CSILK_ERR_TIMEOUT = -6, // 超时
    CSILK_ERR_IO = -7,      // IO 错误
    CSILK_ERR_TLS = -8,     // TLS 错误
    CSILK_ERR_JSON = -9,    // JSON 错误
    CSILK_ERR_MQ = -10,     // 消息队列错误
} csilk_ret_t;
```

### 1.2 错误码映射

```c
// 系统错误到 csilk 错误码
static csilk_ret_t errno_to_csilk(int err) {
    switch (err) {
        case ENOMEM:  return CSILK_ERR_MEMORY;
        case EINVAL:  return CSILK_ERR_INVALID;
        case ENOENT:  return CSILK_ERR_NOT_FOUND;
        case EBUSY:   return CSILK_ERR_BUSY;
        case ETIMEDOUT: return CSILK_ERR_TIMEOUT;
        default:      return CSILK_ERR;
    }
}
```

---

## 2. 日志系统

### 2.1 日志级别

```c
typedef enum csilk_log_level_e {
    CSILK_LOG_TRACE = 0,  // 详细调试信息
    CSILK_LOG_DEBUG = 1,  // 调试信息
    CSILK_LOG_INFO = 2,   // 一般信息
    CSILK_LOG_WARN = 3,   // 警告
    CSILK_LOG_ERROR = 4,  // 错误
    CSILK_LOG_FATAL = 5,  // 致命错误
    CSILK_LOG_OFF = 6     // 关闭
} csilk_log_level_t;
```

### 2.2 日志宏

```c
// 使用示例
CSILK_LOG_TRACE("Detailed trace: x=%d, y=%s", x, y);
CSILK_LOG_DEBUG("Debug info");
CSILK_LOG_INFO("Info message");
CSILK_LOG_WARN("Warning: connection pool exhausted");
CSILK_LOG_ERROR("Error: failed to allocate memory");
CSILK_LOG_FATAL("Fatal: assertion failed");
```

### 2.3 异步日志

```c
// 日志消费者线程
static void* logger_consumer_loop(void* arg) {
    csilk_logger_t* logger = (csilk_logger_t*)arg;
    
    while (logger->running) {
        // 从环缓冲区读取日志
        csilk_log_entry_t entry;
        if (csilk_ringbuf_dequeue(&logger->buf, &entry)) {
            // 写入文件/终端
            fprintf(logger->output, "[%s] %s\n", 
                    entry.timestamp, entry.message);
        }
    }
    return NULL;
}
```

### 2.4 配置

```yaml
logging:
  level: info           # trace/debug/info/warn/error/fatal/off
  output: stdout        # stdout/file/stderr
  file: /var/log/csilk.log
  format: json          # text/json
  async: true           # 异步写入
  max_queue_size: 4096  # 日志队列深度
```

---

## 3. 异常处理

### 3.1 panic 恢复

```c
// 使用 setjmp/longjmp 实现 panic 恢复
#include <setjmp.h>

typedef struct csilk_panic_recovery_s {
    jmp_buf jump_buffer;
    int recovered;
    char error_msg[256];
} csilk_panic_recovery_t;

// 注册 panic 处理器
void csilk_panic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_panic_msg, sizeof(g_panic_msg), fmt, args);
    va_end(args);
    
    longjmp(g_panic_jmpbuf, 1);
}

// 安全调用包装
int csilk_safe_call(int (*func)(void*), void* arg) {
    if (setjmp(g_panic_jmpbuf) == 0) {
        return func(arg);  // 正常执行
    } else {
        // panic 发生，清理并返回错误
        CSILK_LOG_ERROR("Panic recovered: %s", g_panic_msg);
        return CSILK_ERR;
    }
}
```

### 3.2 Deferred Cleanup

```c
// RAII 风格的清理 API
typedef void (*csilk_defer_fn)(void*);

typedef struct csilk_defer_node_s {
    csilk_defer_fn fn;
    void* arg;
    struct csilk_defer_node_s* next;
} csilk_defer_node_t;

// 注册清理函数
void csilk_ctx_defer(csilk_ctx_t* c, csilk_defer_fn fn, void* arg) {
    csilk_defer_node_t* node = csilk_arena_alloc(c->arena, sizeof(*node));
    node->fn = fn;
    node->arg = arg;
    node->next = c->defer_list;
    c->defer_list = node;
}

// 执行清理
void csilk_ctx_cleanup(csilk_ctx_t* c) {
    while (c->defer_list) {
        csilk_defer_node_t* node = c->defer_list;
        c->defer_list = node->next;
        node->fn(node->arg);
        // node 内存由 arena 自动管理
    }
}
```

---

## 4. 调试工具

### 4.1 Admin Dashboard

```
/admin/debug
├── /healthz          - 健康检查
├── /stats            - 服务器统计
├── /connections      - 活跃连接列表
├── /flamegraph       - CPU 火焰图
├── /allocations      - 内存分配统计
└── /config           - 当前配置
```

### 4.2 内存分析

```c
// arena_alloc_debug - 调试分配
#ifdef DEBUG_ARENA
void* arena_alloc_debug(csilk_arena_t* arena, size_t size, 
                         const char* file, int line) {
    void* ptr = csilk_arena_alloc(arena, size);
    
    // 记录分配信息
    track_allocation(ptr, size, file, line);
    
    // 填充红区检测溢出
    arena_fill_redzone(ptr, size);
    
    return ptr;
}
#endif
```

### 4.3 火焰图采样

```c
// 后台采样线程
static void* flamegraph_sampler(void* arg) {
    while (sampling_enabled) {
        // 获取调用栈
        void* buffer[64];
        int nptrs = backtrace(buffer, 64);
        
        // 解码并记录
        char** symbols = backtrace_symbols(buffer, nptrs);
        for (int i = 0; i < nptrs; i++) {
            record_sample(symbols[i]);
        }
        
        usleep(10000);  // 10ms 采样间隔
    }
    return NULL;
}
```

---

## 5. 常见问题排查

### 5.1 连接泄漏

```c
// 检测未关闭的连接
static void check_connection_leaks(worker_pool_t* wp) {
    for (int i = 0; i < wp->active_clients_count; i++) {
        csilk_client_t* c = wp->active_clients[i];
        if (c && c->state == CONNECTION_STATE_ESTABLISHED) {
            time_t age = time(NULL) - c->connected_at;
            if (age > 300) {  // 超过 5 分钟
                CSILK_LOG_W("Potential leak: connection age=%ds", age);
            }
        }
    }
}
```

### 5.2 内存泄漏

```bash
# ASAN 检测
export ASAN_OPTIONS=detect_leaks=1
./build/test_oom
```

```
==12345==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 256 byte(s) in 1 object(s) allocated from:
    #0 malloc interop.c:55
    #1 csilk_arena_new src/core/primitives/arena.c:100
```

### 5.3 数据竞争

```bash
# TSAN 检测
export TSAN_OPTIONS="history_size=7"
./build/test_concurrent
```

```
WARNING: ThreadSanitizer: data race
Read of size 4 at 0x7f... by thread T2:
    #0 worker_thread src/core/server/server_worker.c:50
    
Previous write of size 4 at 0x7f... by main thread:
    #0 csilk_server_set_max_connections src/core/server/server_lifecycle.c:200
```

---

## 6. 调试标志

```c
// 编译时启用调试
-DDEBUG_ARENA      # Arena 红区检测
-DDEBUG_IO         # IO 操作详细日志
-DDEBUG_HTTP       # HTTP 解析详细日志
-DDEBUG_ROUTER     # 路由匹配详细日志
-DDEBUG_MQ         # 消息队列详细日志
```

---

## 7. 运行时诊断

### 7.1 状态查询 API

```c
// /debug/stats
csilk_server_get_stats(server, &active, &pooled);
printf("Active: %d, Pooled: %d\n", active, pooled);

// /debug/arena
size_t used, total;
csilk_arena_stats(&used, &total);
printf("Arena: %zu/%zu bytes\n", used, total);
```

### 7.2 性能计数器

```c
// 原子计数器，零开销查询
extern atomic_uint_fast64_t g_requests_total;
extern atomic_uint_fast64_t g_errors_total;
extern atomic_uint_fast64_t g_active_connections;

// Prometheus 导出
void expose_metrics(csilk_app_t* app) {
    csilk_router_add(app->router, "GET", "/metrics", ...);
}
```

---

## 8. 日志配置示例

```yaml
logging:
  level: debug
  output: file
  file: /var/log/csilk/app.log
  format: json
  async: true
  max_queue_size: 8192
  rotation:
    max_size_mb: 100
    max_files: 10
    compress: true
```

```json
{"timestamp":"2026-08-21T10:00:00Z","level":"info","msg":"Server started","port":8080}
{"timestamp":"2026-08-21T10:00:01Z","level":"debug","msg":"Request received","method":"GET","path":"/api/users"}
```
