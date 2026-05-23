# csilk 架构白皮书

> **Last updated**: 2026-05-23 | **Version**: 0.2.0

## 1. 核心架构设计

csilk 采用了经典的 **Reactor 事件驱动模型**，结合了类似 Go 语言 Gin 框架的 **洋葱模型 (Onion Model)** 中间件机制。

### 1.1 事件驱动底座
框架底层基于 `libuv`。
- 所有网络 I/O 均为非阻塞。
- 使用 `llhttp` 进行状态机驱动的 HTTP 协议解析，确保了极高的解析效率和极低的内存占用。

### 1.2 洋葱模型中间件
中间件通过 `csilk_next()` 机制实现：
1. 请求进入，按序触发中间件的前置逻辑。
2. 调用 `csilk_next()` 进入下一个中间件或业务 Handler。
3. 业务逻辑处理完成后，控制流沿原路返回，触发中间件的后置逻辑（如日志记录、响应耗时统计）。

### 1.3 崩溃恢复机制 (Recovery)
利用 C 语言的 `setjmp` 和 `longjmp` 实现了轻量级的异常捕获：
- 在请求入口点（Recovery 中间件）设置 `setjmp` 锚点。
- 当 Handler 中调用 `csilk_panic()` 时，通过 `longjmp` 瞬间跳回锚点。
- 框架会自动清理请求资源并返回 500 错误，确保单个请求的崩溃不会导致整个服务器进程退出。

## 2. 内存管理策略

### 2.1 请求级内存池 (Arena Allocator)
为了解决高并发下大量小内存分配导致的堆碎片和锁竞争问题，csilk 引入了 **Arena Allocator**：
- 每一条 HTTP 连接在开始处理请求时，都会分配一个初始大小为 4KB 的 Arena 块。
- 请求生命周期内的所有临时字符串（如 `csilk_string`）、URL 参数、解析出的 Headers 均从 Arena 中分配。
- **优点**: 内存分配仅为指针偏移，速度极快；请求结束时一键释放整个 Arena，彻底杜绝内存泄漏。

## 3. 路由引擎：Radix Tree

路由系统采用 **前缀树 (Radix Tree)** 实现：
- 相比于正则匹配或线性匹配，Radix Tree 在路由数量增加时仍能保持 O(K) 的匹配复杂度（K 为路径长度）。
- **优化**: 节点子节点采用固定数组布局（`CSILK_MAX_CHILDREN`），利用 CPU L1/L2 缓存局部性，进一步提升匹配性能。
- **动态特性**: 原生支持 `:param` 命名参数和 `*wildcard` 通配符。

## 4. WebSocket 支持

csilk 原生支持 WebSocket 协议升级：
- **握手逻辑**: 实现了符合 RFC 6455 的 SHA1 + Base64 握手算法。
- **协议接管**: 升级成功后，框架会自动将 libuv 的读回调从 HTTP 解析器切换到 WebSocket 帧解析器。
- **异步接口**: 提供 `csilk_ws_send` 异步发送帧，并通过 `on_ws_message` 回调处理接收到的数据。

## 5. 文档生成

csilk 使用 **Doxygen** 生成 API 文档：
- 所有公共头文件（`include/`）包含完整的 `@brief`、`@param`、`@return` 注解。
- 所有实现文件（`src/`）包含 `@file`、`@brief` 头部注释，关键函数有 `@brief` 和 `@param` 文档。
- 示例代码（`examples/`）同样包含 Doxygen 注解。
- 文档生成命令：`make docs`（需要 Doxygen 1.12+）。
- CI 中配置了 GitHub Pages 自动部署生成的 HTML 文档。

## 6. 开发者指南

### 5.1 编写 WebSocket 处理器
```c
void ws_on_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    csilk_ws_send(c, (uint8_t*)"Hello Client", 12, 1);
}

void ws_handler(csilk_ctx_t* c) {
    csilk_ws_handshake(c);
    if (c->is_websocket) {
        c->on_ws_message = ws_on_message;
    }
}
```

### 5.2 编写中间件
```c
void my_middleware(csilk_ctx_t* c) {
    // 前置逻辑：如检查 Token
    csilk_next(c);
    // 后置逻辑：如记录日志
}
```

### 5.3 启动服务
```c
int main() {
    csilk_router_t* r = csilk_router_new();
    csilk_group_t* g = csilk_group_new(r, "/api");
    csilk_GET(g, "/ping", handler);
    
    csilk_server_t* s = csilk_server_new(r);
    csilk_server_run(s, 8080);
    return 0;
}
```
