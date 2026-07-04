# 消息队列 / 事件总线模块

## 1. 概述

Messaging 模块提供了一个 **进程内发布/订阅事件总线**（消息队列）用于 csilk 服务器内的组件之间解耦通信。它通过基于主题的消息交换实现路由、中间件、AI 工作流、WebSocket 处理器和自定义模块之间的通信。发布 **MUST** 为单生产者场景锁自由；多生产者发布 **MUST** 使用原子 CAS。订阅处理器 **MUST NOT** 阻塞 — 长时间运行的工作 **SHOULD** 分发到 libuv 线程池。消息传递 **MUST** 默认为最多一次语义；确切一次 **SHOULD** 通过 WAL 持久化和 ACK 跟踪选择加入。每个主题订阅分发开销 **SHOULD** 每订阅链链接 ≤ 200ns。

**文件**: `src/messaging/`, `include/csilk/mq.h`

---

## 2. 架构

```
┌────────────────────────────────────────────────────────┐
│                    Publisher (any thread)                │
│              csilk_mq_publish(mq, topic, data, len)      │
└────────────────────────┬───────────────────────────────┘
                         │
                         ▼
┌────────────────────────────────────────────────────────┐
│                    MQ Instance (csilk_mq_t)              │
│                                                          │
│  ┌──────────────┐    ┌──────────────────────────────┐   │
│  │  WAL (disk)  │◄───│  Publish Path:               │   │
│  │  ── append   │    │  1. _mq_append_wal() (opt.)  │   │
│  │  ── fsync    │    │  2. _mq_enqueue() → queue LL │   │
│  └──────────────┘    │  3. uv_async_send()          │   │
│                       └──────────┬───────────────────┘   │
│                                  │ async signal           │
│                       ┌──────────▼───────────────────┐   │
│                       │  Dispatch (main loop):        │   │
│                       │  on_mq_async() →              │   │
│                       │  1. Dequeue all messages      │   │
│                       │  2. For each msg:             │   │
│                       │     a. Build handler chain    │   │
│                       │     b. Run chain (middleware  │   │
│                       │        → subscribers)         │   │
│                       │     c. Free message           │   │
│                       └──────────────────────────────┘   │
│                                                          │
│  ┌──────────────┐    ┌──────────────────────────────┐   │
│  │ Monitors     │    │  Topic Registry:             │   │
│  │ (WebSocket)  │◄───│  topics → linked list        │   │
│  │ mq_published │    │  topic → handlers[]          │   │
│  │ mq_delivered │    │  global_middlewares[]         │   │
│  └──────────────┘    └──────────────────────────────┘   │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │  Background Offload (libuv thread pool):          │   │
│  │  csilk_mq_offload() → uv_queue_work()             │   │
│  └──────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────┘
```

### 关键设计决策

1. **仅进程内** — 消息从不离开进程边界。这避免序列化开销、网络延迟，以及外部消息代理（Kafka、RabbitMQ、Redis Streams）的操作复杂性。对于跨进程消息传递，可使用 Redis 或专用代理作为数据库驱动。

2. **线程安全发布通过 `uv_async_t`** — 来自任何工作线程的发布者在互斥锁下入队消息并通过 `uv_async_send()` 向主循环发信号。所有分发（处理器链执行）在主事件循环线程上发生，消除订阅代码中的并发性。

3. **中间件优先分发** — 每个主题都有一个从全局中间件 → 主题中间件 → 订阅者的处理器链。中间件可以短路链 (`csilk_mq_abort()`), 传递控制 (`csilk_mq_next()`), 或离线工作到线程池。

4. **可选 WAL 持久化** — 当启用时，每个发布持久写入磁盘。在重启时，WAL 被重放以恢复未传递的消息。

---

## 3. 核心数据结构

### 3.1 `csilk_mq_msg_t` — 单个入队消息

```c
struct csilk_mq_msg_s {
    char* topic;        // 堆分配的主题字符串副本
    void* payload;      // 堆分配的有效负载副本
    size_t len;          // 有效负载字节长度
    csilk_mq_msg_t* next; // 链表指针 (nullptr for tail)
};
```

消息在发布时深度复制 — 调用者可以在返回后立即重用/释放原始缓冲区。

### 3.2 `csilk_mq_topic_t` — 注册主题节点

```c
struct csilk_mq_topic_s {
    char* name;               // 主题名称（例如 "user.created"）
    csilk_mq_handler_t* handlers; // 动态增长的处理器数组
    size_t handler_count;      // 当前处理器数
    size_t handler_capacity;   // 分配容量
    csilk_mq_topic_t* next;   // 链表指针
};
```

主题在第一次 `subscribe()` 或 `use()` 调用时延迟创建。主题名称支持 `fnmatch(3)` 全局模式。

### 3.3 `csilk_mq_ctx_t` — 每消息执行上下文

```c
struct csilk_mq_ctx_s {
    csilk_mq_t* mq;             // 所属 MQ 实例
    csilk_mq_msg_t* msg;        // 正在处理的消息
    csilk_mq_handler_t* handlers; // 解析的处理器链
    size_t handler_count;        // 链中的处理器总数
    int handler_index;           // 当前位置
    int aborted;                 // 非零如果 abort() 被调用
};
```

为每个消息在分发时创建。仅在处理器调用期间有效。

### 3.4 `csilk_mq_t` — MQ 实例

```c
struct csilk_mq_s {
    uv_async_t async_handle;       // 主循环信号桥
    uv_mutex_t queue_mutex;        // 保护消息链表
    csilk_mq_msg_t* queue_head;    // 等待消息队列 (FIFO)
    csilk_mq_msg_t* queue_tail;

    csilk_mq_topic_t* topics;      // 注册主题列表

    csilk_mq_handler_t* global_middlewares; // 拦截每个主题
    size_t global_mw_count;
    size_t global_mw_capacity;

    uv_file wal_fd;                // WAL 文件描述符 (-1 如果禁用)
    char* wal_path;                // WAL 文件路径
    uv_mutex_t wal_mutex;          // 保抎 WAL 写入

    uint64_t published_total;      // 统计
    uint64_t delivered_total;
    uint64_t failed_total;
    uint32_t queue_depth;

    csilk_ctx_t** monitors;        // WebSocket 监控连接
    size_t monitor_count;
    size_t monitor_capacity;
    uv_mutex_t monitor_mutex;
};
```

---

## 4. API 界面

### 发布

| 函数 | 角色 |
|---|---|
| `csilk_mq_publish(mq, topic, payload, len)` | 深度复制有效负载 → 可选 WAL 追加 → 入队 → 信号主循环 |

### 订阅

| 函数 | 角色 |
|---|---|
| `csilk_mq_subscribe(mq, topic, handler)` | 为主题注册订阅者 |
| `csilk_mq_use(mq, topic, handler)` | 注册中间件 (nullptr 主题 = 全局) |

### 链控制（在处理器内）

| 函数 | 角色 |
|---|---|
| `csilk_mq_next(ctx)` | 推进到链中的下一个处理器 |
| `csilk_mq_abort(ctx)` | 短路整个链 |
| `csilk_mq_get_topic(ctx)` | 获取当前消息的主题字符串 |
| `csilk_mq_get_payload(ctx, &len)` | 获取当前消息的有效负载指针 |

### 后台离线

| 函数 | 角色 |
|---|---|
| `csilk_mq_offload(ctx, worker)` | 深度复制消息 → 分发到线程池 → 继续链 |

### 持久化

| 函数 | 角色 |
|---|---|
| `csilk_mq_set_persistence(mq, wal_path)` | 启用 WAL — 打开文件，恢复过去的消息 |
| `_mq_append_wal(mq, topic, payload, len)` | 内部: 追加带 XOR 校验和的帧 |
| `_mq_recovery(mq)` | 内部: 读取 WAL 帧，验证校验和，重新入队 |

### 监控 & 指标

| 函数 | 角色 |
|---|---|
| `csilk_mq_get_stats(mq, stats*)` | 读取 published/delivered/failed/queue_depth/topic_count |
| `csilk_mq_stats_to_json(stats*)` | 将指标转换为 JSON 字符串 |
| `csilk_mq_register_monitor(mq, ctx)` | 注册 WebSocket 连接接收实时 MQ 事件 |
| `csilk_server_get_mq(server)` | 获取或延迟创建服务器的 MQ 实例 |

---

## 5. 分发算法

```
on_mq_async(handle):
  1. 加锁 queue_mutex
  2. 分离整个队列 (head → nullptr, tail → nullptr, 保存计数)
  3. 解锁 queue_mutex
  4. For each message (draining the linked list):

     a. 构建处理器链:
        - 分配 chain[] = malloc((global_mw_count + matched_topic_handlers) * sizeof(handler_t))
        - 将 global_middlewares 复制到 chain[]
        - For each topic t in mq->topics:
             if fnmatch(t->name, msg->topic) == 0:
                 memcpy chain[] with t->handlers[]

     b. 创建堆栈上下文:
        csilk_mq_ctx_t ctx = {mq, msg, chain, total_handlers, -1 /* index */, 0 /* aborted */}

     c. 开始链:
        csilk_mq_next(&ctx)
        // handler_index 变为 0 → 调用 chain[0](&ctx)
        // 每个处理器可选调用 csilk_mq_next(&ctx) 推进

     d. 释放链分配
     e. 释放消息 (topic, payload, msg struct)
```

### 主题匹配

主题名称支持 `fnmatch(3)` 全局模式。这意味着：

- `"user.*"` 匹配 `"user.created"`、`"user.deleted"` 等
- `"*"` 匹配所有主题
- `"events.*.completed"` 匹配 `"events.order.completed"`

### 处理器链执行顺序

```
For message on topic "events.user.created":

  1. Global middleware #0   → csilk_mq_next() 推进到 #1
  2. Global middleware #1   → csilk_mq_next() 推进到 #2
  ...
  N. Topic "events.*" middleware #0    (fnmatch 匹配)
  M. Topic "events.user.*" middleware   (更具体的匹配)
  O. Topic "events.user.created" subscriber #0
  P. Topic "events.user.created" subscriber #1
```

任何处理器可调用 `csilk_mq_abort()` 停止链或 `csilk_mq_offload()` 将处理离线到线程池。

---

## 6. WAL 持久化

### 帧格式

```
+0: topic_len   (uint32_t, 4 bytes, LE)
+4: topic       (topic_len bytes, NOT null-terminated on disk)
+N: payload_len (uint32_t, 4 bytes)
+M: payload     (payload_len bytes)
+K: checksum    (uint32_t, 4 bytes, XOR of topic + payload bytes)
```

**开销**: 每消息 12 字节（2× uint32 长度字段 + 1× uint32 校验和）。

### 写入路径

```
csilk_mq_publish()
  ├── _mq_append_wal()     ← WAL 写入 (wal_mutex 下)
  │     ├── 计算 XOR 校验和在 topic + payload 字节上
  │     ├── uv_fs_write()  ← 5 个缓冲区的散集/收集
  │     └── uv_fs_fsync()  ← 持久性屏障
  └── _mq_enqueue()        ← 内存队列
```

WAL 写入是在调用线程上**同步**的 — 它阻塞直到 fsync 完成。这以吞吐量交换耐久性。对于高吞吐量场景，批量写入是优化机会。

### 恢复路径

```
csilk_mq_set_persistence()
  ├── Open/Create WAL file (O_CREAT | O_RDWR | O_APPEND)
  └── _mq_recovery()
        └── 从偏移 0 开始顺序位置读取:
              for each frame:
                1. 读取 topic_len (4 bytes)
                2. 读取 topic     (topic_len bytes)
                3. 读取 payload_len (4 bytes)
                4. 读取 payload     (payload_len bytes)
                5. 读取 stored_checksum (4 bytes)
                6. 计算 XOR 校验和在 topic + payload 上
                7. 如果匹配 → _mq_enqueue() (重新传递)
                8. 如果不匹配 → 停止 (损坏边界)
```

在校验和不匹配时，恢复 **停止** 而不是尝试重新同步 — 如果不在头中有帧长度前缀，下一个帧边界无法确定。WAL 在恢复后不截断；新附加继续在当前文件偏移处。

### 完整性

XOR 校验和检测单比特翻转和许多破坏模式，但 **不是加密**。它保护 against accidental corruption（磁盘错误、部分写入），不保护 against 恶意篡改。

---

## 7. 后台离线

```
csilk_mq_offload(ctx, worker_function):
  1. 分配 csilk_mq_work_ctx_t
  2. strdup topic, malloc+memcpy payload (深度复制)
  3. uv_queue_work(loop, &wctx->req, worker_cb, worker_after_cb)
     │                              │                │
     │  worker_cb()                 │                │
     │  └─ 在 THREAD POOL 上运行      │                │
     │  └─ 调用用户 worker()         │                │
     │     与深度复制              │                │
     │                              │                │
     │              worker_after_cb()                │
     │              └─ 在 MAIN LOOP 上运行         │
     │              └─ 释放深度复制 + wctx      │
     │                                                                         │
     └── csilk_mq_next(ctx)  ← 链继续
                                   立即在主
                                   循环线程
```

这启用后台处理而不阻塞处理器链。工作者函数接收深度复制的 topic 和 payload 的独占所有权，不得调用 MQ 或上下文 API。

---

## 8. 监控

WebSocket 客户端可通过 `csilk_mq_register_monitor(mq, ctx)` 注册为 MQ 监控。监控接收 JSON 事件：

| 事件 | 触发 | 字段 |
|---|---|---|
| `mq_published` | 消息入队 | topic, payload_len, timestamp |
| `mq_delivered` | 消息被分发 | topic, payload_len, timestamp |

事件作为 WebSocket 文本帧广播到所有注册的监控。由 `monitor_mutex` 保护。这为管理仪表板的实时 MQ 监控页面提供动力。

---

## 9. 并发模型

```
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  Worker Thread  │  │  Worker Thread  │  │  Worker Thread  │
│  publish()      │  │  publish()      │  │  publish()      │
└────────┬────────┘  └────────┬────────┘  └────────┬────────┘
         │                    │                    │
         │    queue_mutex     │                    │
         └────────────────────┼────────────────────┘
                            │
                            ▼
               ┌──────────────────┐
               │  Pending Queue   │
               │  (linked list)    │
               └────────┬─────────┘
                        │ uv_async_send()
                        ▼
               ┌──────────────────┐
               │  Main Loop       │
               │  on_mq_async()   │
               │  └─ dispatch()   │
               └──────────────────┘
```

- **发布**: 通过 `queue_mutex` 线程安全。仅在链表追加和可选 WAL 写入期间阻塞。
- **分发**: 单线程在主事件循环上。处理器执行期间不持有锁。
- **WAL**: 由 `wal_mutex` 序列化。在每个 `write + fsync` 持有锁。
- **指标**: 由 `queue_mutex` 保护。在锁下更新原子。
- **监控**: 由 `monitor_mutex` 保护。在锁下广播。

---

## 10. 生命周期

```
Server startup:
  csilk_server_get_mq(server)
    └── _csilk_mq_new(loop)
          ├── calloc csilk_mq_t
          ├── uv_mutex_init(queue_mutex, monitor_mutex, wal_mutex)
          ├── uv_async_init(loop, async_handle, on_mq_async)
          └── wal_fd = -1

Runtime:
  csilk_mq_subscribe/use       → 懒创建主题，注册处理器
  csilk_mq_publish             → WAL-追加 + 入队 + 异步信号
  csilk_mq_set_persistence     → 打开 WAL 文件 + 恢复重放
  csilk_mq_register_monitor    → 添加 WebSocket 客户端

Server shutdown:
  _csilk_mq_free(mq)
    └── uv_close(async_handle, on_mq_close)
          ├── uv_mutex_destroy(queue_mutex, wal_mutex)
          ├── 关闭 WAL 文件
          ├── 排空消息队列 (释放所有消息)
          ├── 释放所有主题 + 处理器数组
          ├── 释放全局中间件
          └── free(mq)
```

---

## 11. 使用示例

### C — 基本 pub/sub

```c
csilk_mq_t* mq = csilk_server_get_mq(server);

// 订阅者
void on_user_created(csilk_mq_ctx_t* ctx) {
    const char* topic = csilk_mq_get_topic(ctx);
    size_t len;
    const void* payload = csilk_mq_get_payload(ctx, &len);
    printf("Received on %s: %.*s\n", topic, (int)len, (char*)payload);
}
csilk_mq_subscribe(mq, "user.created", on_user_created);

// 发布者 (从任何线程)
csilk_mq_publish(mq, "user.created", "hello", 6);
```

### C — 中间件链与 abort

```c
void validator(csilk_mq_ctx_t* ctx) {
    size_t len;
    const char* data = (const char*)csilk_mq_get_payload(ctx, &len);
    if (len == 0) {
        csilk_mq_abort(ctx);  // 拒绝空消息
        return;
    }
    csilk_mq_next(ctx);
}

void logger(csilk_mq_ctx_t* ctx) {
    printf("Processing: %s\n", csilk_mq_get_topic(ctx));
    csilk_mq_next(ctx);
}

csilk_mq_use(mq, "orders.*", validator);   // 全局类似通过模式
csilk_mq_use(mq, "orders.*", logger);
csilk_mq_subscribe(mq, "orders.new", handler);
```

### C — 后台离线

```c
void heavy_work(const char* topic, const void* payload, size_t len) {
    // 在线程池运行
    process_data(payload, len);
}

void on_message(csilk_mq_ctx_t* ctx) {
    csilk_mq_offload(ctx, heavy_work);
    // 链继续立即；heavy_work 在后台运行
}
```

### C — WAL 持久化

```c
csilk_mq_t* mq = csilk_server_get_mq(server);
csilk_mq_set_persistence(mq, "/var/lib/csilk/mq.wal");
// 从 WAL 恢复过去的消息，未来的消息持久存储
```

### Python (ctypes 绑定)

```python
from csilk import App

app = App()
mq = app.mq

@mq.subscribe("events.user.created")
def handle_created(ctx):
    print(f"Got: {ctx.topic}")
    ctx.next()

@mq.use("*")  # 全局中间件
def log_all(ctx):
    print(f"Before: {ctx.topic}")
    ctx.next()

mq.publish("events.user.created", b"hello")
```

---

## 12. 相关文件

| 文件 | 角色 |
|---|---|
| `src/messaging/mq_core.c` | MQ 创建、销毁、指标、监控注册 |
| `src/messaging/mq_dispatch.c` | 消息入队、发布、异步分发、链执行 |
| `src/messaging/mq_pubsub.c` | 订阅、use (中间件)，主题延迟创建 |
| `src/messaging/mq_context.c` | 每消息上下文: next, abort, get_topic, get_payload |
| `src/messaging/mq_offload.c` | 后台线程池离线 |
| `src/messaging/mq_wal.c` | WAL 追加、恢复、set_persistence |
| `src/messaging/mq_internal.h` | 内部结构定义和跨文件助手 |
| `include/csilk/mq.h` | 公共 API 用于 pub/sub, 离线, 持久化, 监控 |
| `src/messaging/internal/` | 附加内部助手 |
| `python/csilk/app.py` | Python MqContext 和 MQ 绑定类 |

---

## 13. 设计理由

**为什么使用链表主题注册表而不是哈希表？**  
主题注册表通常很小（通常几十个主题，而不是几千个）。链表迭代是分发热路径中缓存友好的（扫描所有主题进行 fnmatch 匹配），并且实现是微不足道的，没有调整大小或碰撞复杂性。

**为什么使用 fnmatch 而不是精确字符串匹配？**  
全局模式启用层次主题命名空间（例如，`"events.*"`、`"orders.*.completed"`），这对于应用于广泛消息类别的中间件是必需的。`fnmatch` 的性能成本在进程内是微不足道的。

**为什么要使用 WAL 而不是仅内存设计？**  
WAL 是可选的。在内存模式下，有零持久化开销。WAL 可用于消息传递必须在进程重启后生存的场景 — 在入队前持久写入，因此在入队和分发之间崩溃不会丢失消息。

**为什么在发布时深度复制？**  
调用者拥有有效负载缓冲区，并可在 `csilk_mq_publish()` 返回后立即释放。深度复制确保 MQ 拥有自己的数据，避免 use-after-free 错误，在每个发布的成本是一次额外分配。