# 消息队列 (MQ) 使用指南

> **Version**: 0.3.0 | **Last updated**: 2026-06-29

csilk 的内置消息队列（Message Queue, MQ）是一个基于 libuv `uv_async_t` 的进程内发布/订阅事件总线。支持主题路由、中间件链、后台 offload、WAL 持久化，以及 WebSocket 实时监控。

**核心特性**：

- **线程安全发布** — 任意线程均可通过 `csilk_mq_publish` 安全发布消息
- **异步投递** — 消息在主事件循环上异步处理，不会阻塞发布者
- **中间件链** — 支持全局和主题级别的中间件，与 HTTP 中间件相似的洋葱模型
- **后台 Offload** — 将耗时消息处理移交到 libuv 线程池
- **WAL 持久化** — 写前日志支持崩溃恢复
- **WebSocket 监控** — 通过 Admin Dashboard 实时查看队列状态

---

## 1. 快速开始

### 1.1 获取 MQ 实例

```c
#include "csilk/csilk.h"

int main() {
    csilk_router_t* r = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r);

    // MQ 在首次访问时延迟创建
    csilk_mq_t* mq = csilk_server_get_mq(s);
    if (!mq) {
        CSILK_LOG_E("Failed to get MQ instance");
        return 1;
    }

    // 注册订阅者
    csilk_mq_subscribe(mq, "user.created", on_user_created);
    csilk_mq_subscribe(mq, "order.placed", on_order_placed);

    csilk_server_run(s, 8080);
    csilk_server_free(s);
    return 0;
}
```

### 1.2 订阅者处理函数

```c
void on_user_created(csilk_mq_ctx_t* ctx) {
    // 获取消息主题
    const char* topic = csilk_mq_get_topic(ctx);
    CSILK_LOG_I("Received message on topic: %s", topic);

    // 获取消息负载
    size_t len = 0;
    const void* payload = csilk_mq_get_payload(ctx, &len);

    // 解析 JSON 负载
    cJSON* json = cJSON_ParseWithLength((const char*)payload, len);
    if (json) {
        const char* user_id = cJSON_GetObjectItem(json, "id")->valuestring;
        CSILK_LOG_I("New user created: %s", user_id);
        cJSON_Delete(json);
    }
}
```

### 1.3 发布消息

```c
void create_user_handler(csilk_ctx_t* c) {
    // 业务逻辑...
    csilk_mq_t* mq = csilk_server_get_mq(csilk_get_server(c));

    // 构造消息
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "id", "user_123");
    cJSON_AddStringToObject(msg, "email", "alice@example.com");
    char* json_str = cJSON_PrintUnformatted(msg);

    // 发布到 "user.created" 主题（线程安全，内部复制负载）
    csilk_mq_publish(mq, "user.created", json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(msg);

    csilk_string(c, 200, "User created");
}
```

---

## 2. 消息中间件

MQ 中间件在订阅者之前执行，支持全局和主题两种作用域。

### 2.1 全局中间件

拦截所有主题的消息：

```c
void mq_logger_middleware(csilk_mq_ctx_t* ctx) {
    const char* topic = csilk_mq_get_topic(ctx);
    CSILK_LOG_I("[MQ] Processing: %s", topic);

    // 调用下一个中间件或订阅者
    csilk_mq_next(ctx);

    CSILK_LOG_I("[MQ] Done: %s", topic);
}

void setup(csilk_mq_t* mq) {
    // 注册全局 MQ 中间件（topic 传 nullptr）
    csilk_mq_use(mq, nullptr, mq_logger_middleware);
}
```

### 2.2 主题中间件

仅拦截特定主题：

```c
void order_validator_middleware(csilk_mq_ctx_t* ctx) {
    size_t len = 0;
    const char* payload = (const char*)csilk_mq_get_payload(ctx, &len);

    cJSON* json = cJSON_ParseWithLength(payload, len);
    if (!json || !cJSON_GetObjectItem(json, "order_id")) {
        CSILK_LOG_E("Invalid order message, aborting");
        csilk_mq_abort(ctx);  // 中止链，不执行订阅者
        cJSON_Delete(json);
        return;
    }
    cJSON_Delete(json);

    csilk_mq_next(ctx);  // 验证通过，继续
}

void setup(csilk_mq_t* mq) {
    // 只对 "order.*" 相关主题生效
    csilk_mq_use(mq, "order.placed", order_validator_middleware);
    csilk_mq_subscribe(mq, "order.placed", on_order_placed);
}
```

### 2.3 MQ 中间件链流程

```
消息发布到 "order.placed"
  │
  ├─ [全局中间件] mq_logger → csilk_mq_next()
  ├─ [主题中间件] order_validator → csilk_mq_next()
  ├─ [订阅者] on_order_placed
  │
  └─ [回复路径] order_validator (post)
                    mq_logger (post)
```

---

## 3. 后台 Offload

将消息处理移交到 libuv 线程池，避免阻塞事件循环：

```c
void email_worker(const char* topic, const void* payload, size_t len) {
    // 运行在线程池中 — 可以执行阻塞操作
    const char* email_data = (const char*)payload;
    send_email(email_data);  // 可能耗时数秒
    // 注意：不要在此回调中调用 MQ API（非线程安全）
}

void on_send_email(csilk_mq_ctx_t* ctx) {
    // 将任务 offload 到后台线程
    csilk_mq_offload(ctx, email_worker);
    // csilk_mq_next 会自动调用，链继续执行
}
```

---

## 4. WAL 持久化

启用写前日志（Write-Ahead Log）后，每条消息在投递前先写入磁盘文件。服务器重启后可恢复未处理的消息。

### 4.1 启用 WAL

```c
void setup_mq_with_persistence(csilk_mq_t* mq) {
    // 启用 WAL，消息写入 "mq.wal" 文件
    int rc = csilk_mq_set_persistence(mq, "mq.wal");
    if (rc != 0) {
        CSILK_LOG_E("Failed to enable MQ persistence");
    }
}
```

### 4.2 WAL 恢复流程

```c
void start_server_with_mq_recovery(void) {
    csilk_server_t* s = csilk_server_new(router);
    csilk_mq_t* mq = csilk_server_get_mq(s);

    // 1. 注册订阅者（必须在启动前完成）
    csilk_mq_subscribe(mq, "order.placed", on_order_placed);

    // 2. 启用 WAL（如果文件存在，启动时自动重放）
    csilk_mq_set_persistence(mq, "mq.wal");

    // 3. 启动服务器（WAL 会在事件循环开始后重放）
    csilk_server_run(s, 8080);
}
```

> **注意**：WAL 重放是异步的，在服务器启动后按顺序投递之前未处理的持久化消息。

---

## 5. 监控与统计

### 5.1 编程方式获取统计

```c
void print_mq_stats(csilk_mq_t* mq) {
    csilk_mq_stats_t stats;
    csilk_mq_get_stats(mq, &stats);

    CSILK_LOG_I("MQ Stats:");
    CSILK_LOG_I("  Published: %lu", stats.published_total);
    CSILK_LOG_I("  Delivered: %lu", stats.delivered_total);
    CSILK_LOG_I("  Failed:    %lu", stats.failed_total);
    CSILK_LOG_I("  Queue depth: %u", stats.queue_depth);
    CSILK_LOG_I("  Topics:    %u", stats.topic_count);
}

// 将统计信息转为 JSON
void stats_to_response(csilk_ctx_t* c) {
    csilk_mq_t* mq = csilk_server_get_mq(csilk_get_server(c));
    csilk_mq_stats_t stats;
    csilk_mq_get_stats(mq, &stats);

    char* json = csilk_mq_stats_to_json(&stats);
    csilk_string(c, 200, json);
    free(json);
}
```

### 5.2 通过 Admin Dashboard 监控

启用 Admin Dashboard 后，MQ 统计信息自动可视化：

```c
#include "csilk/app/admin.h"

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");
    csilk_admin_serve(app, "/admin");
    // 访问 http://localhost:8080/admin/ 可查看 MQ 面板
    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

---

## 6. 与 WebSocket Rooms 集成

MQ 天然支持 WebSocket 房间广播：

```c
void ws_chat_handler(csilk_ctx_t* c) {
    // 执行 WebSocket 握手
    csilk_ws_handshake(c);

    // 加入聊天室
    csilk_ws_join_room(c, "chat:general");

    // 设置消息接收回调
    csilk_set(c, "on_ws_message", on_chat_message);
}

void on_chat_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    if (opcode == 0x1) {  // 文本帧
        // 通过 MQ 广播到房间内所有 WebSocket 客户端
        csilk_ws_broadcast_room(c, "chat:general", (const char*)payload);
    }
}
```

---

## 7. 完整示例

一个完整的订单处理系统，结合 MQ 中间件、持久化和后台 offload：

```c
#include "csilk/csilk.h"

// --- 中间件 ---

void mq_audit_log(csilk_mq_ctx_t* ctx) {
    const char* topic = csilk_mq_get_topic(ctx);
    CSILK_LOG_I("[AUDIT] Topic: %s", topic);
    csilk_mq_next(ctx);
}

void order_validation(csilk_mq_ctx_t* ctx) {
    size_t len = 0;
    const char* data = (const char*)csilk_mq_get_payload(ctx, &len);
    cJSON* json = cJSON_ParseWithLength(data, len);

    if (!json || !cJSON_GetObjectItem(json, "amount")) {
        CSILK_LOG_E("Invalid order, rejected");
        csilk_mq_abort(ctx);
        cJSON_Delete(json);
        return;
    }

    cJSON_Delete(json);
    csilk_mq_next(ctx);
}

// --- 后台 Worker ---

void invoice_worker(const char* topic, const void* payload, size_t len) {
    // 在后台线程中生成发票 PDF
    generate_invoice_pdf((const char*)payload);
}

// --- 订阅者 ---

void on_order_placed(csilk_mq_ctx_t* ctx) {
    CSILK_LOG_I("Order received, processing...");
    csilk_mq_offload(ctx, invoice_worker);
    // csilk_mq_next 自动被 offload 调用
}

// --- HTTP 路由 ---

void place_order(csilk_ctx_t* c) {
    cJSON* body = csilk_get_json(c);
    char* json_str = cJSON_PrintUnformatted(body);

    csilk_mq_t* mq = csilk_server_get_mq(csilk_get_server(c));
    csilk_mq_publish(mq, "order.placed", json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(body);
    csilk_json(c, 202, "{\"status\":\"accepted\"}");
}

// --- 主程序 ---

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");

    // 初始化 MQ
    csilk_mq_t* mq = csilk_server_get_mq(csilk_get_server_from_app(app));

    // 全局中间件
    csilk_mq_use(mq, nullptr, mq_audit_log);

    // 主题中间件
    csilk_mq_use(mq, "order.placed", order_validation);

    // 订阅者
    csilk_mq_subscribe(mq, "order.placed", on_order_placed);

    // WAL 持久化
    csilk_mq_set_persistence(mq, "orders.wal");

    // HTTP 路由
    csilk_app_post(app, "/orders", place_order);
    csilk_admin_serve(app, "/admin");

    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

---

## 8. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **SHOULD** 主题使用命名空间 | 例如 `user.created`、`order.placed`，避免命名冲突 |
| **SHOULD** 验证类中间件使用 `csilk_mq_abort` | 消息验证不通过时立即中止链，避免无效消息到达订阅者 |
| **SHOULD** 耗时处理使用 `csilk_mq_offload` | 超过 10ms 的处理应 offload 到线程池 |
| **SHOULD** 关键消息启用 WAL | 订单、支付等不可丢的消息应启用持久化 |
| **MUST NOT** 在 offload worker 中调用 MQ API | 后台线程非线程安全，仅能访问传入的 payload 副本 |
| **MAY** JSON 消息体 | 使用 cJSON 构造结构化的消息负载，便于中间件解析验证 |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — 消息队列](../../docs/module-design/messaging.md) | MQ 内部架构、uv_async_t 分发、WAL 实现细节 |
| [WebSocket Rooms](./advanced-usage.md#websocket-rooms-实现基于-mq) | 基于 MQ 的 WebSocket 房间广播 |
| [Admin Dashboard](./advanced-usage.md#admin-dashboard) | MQ 监控面板使用说明 |
