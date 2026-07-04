# csilk 用户手册

本手册提供关于 csilk 框架配置、扩展和高级使用的详细指南。

## 目录

### 基础指南
- [Configuration](configuration.md): YAML 配置文件结构和选项。
- [Middleware Development](middleware-dev.md): 如何创建和注册自定义中间件。
- [Advanced Usage](advanced-usage.md): 高级路由、上下文、事件总线（MQ）和自定义驱动。

### 安全
- [Security](security.md): JWT 认证、RBAC 权限、CORS、CSRF、WAF 和速率限制。

### AI
- [AI Engine](ai-engine.md): 统一 LLM 接口 — 聊天、SSE 流式传输、嵌入、函数调用和会话上下文。
- [Workflow](workflow.md): 基于图的 AI 管道编排 — DAG 节点、条件路由、工具调用、WAL 持久化和分布式执行。

### 数据库
- [Database](database.md): 统一数据库接口 — SQLite、MySQL、PostgreSQL、MongoDB、Redis 连接池、查询和事务。

### 消息队列
- [Message Queue](message-queue.md): 进程内事件总线 — 发布/订阅消息传递、中间件链、WAL 持久化和监控。

### 部署运维
- [Deployment](deployment.md): Docker、systemd、多工作线程调优、TLS/HTTPS、Prometheus、健康检查和内核调优。

### 系统扩展
- [Reflection](reflection.md): 编译时结构体自省和自动 JSON 序列化/反序列化。
- [Admin Dashboard](admin.md): 基于 Web 的 HTTP、AI、MQ 和数据库指标监控。
- [Hooks](hooks.md): 生命周期钩子系统 — 服务器启动/停止、连接打开/关闭、请求开始/结束。
- [Arena](arena.md):  bump 分配器 — 零碎片化请求作用域内存管理。
- [Hot Reload](hot-reload.md): 无需重启服务器即可实时交换路由。

### Python
- [Python Bindings](python.md): Python 绑定的安装和 API 参考。
