# csilk User Manual

This manual provides detailed guides on configuring, extending, and using advanced features of the csilk framework.

## Table of Contents

### 基础指南
- [Configuration](configuration.md): YAML configuration file structure and options.
- [Middleware Development](middleware-dev.md): How to create and register custom middleware.
- [Advanced Usage](advanced-usage.md): Advanced routing, contexts, event bus (MQ), and custom drivers.

### 安全
- [Security](security.md): JWT authentication, RBAC permissions, CORS, CSRF, WAF, and rate limiting.

### AI
- [AI Engine](ai-engine.md): Unified LLM interface — chat, streaming via SSE, embeddings, function calling, and conversation context.
- [Workflow](workflow.md): Graph-based AI pipeline orchestration — DAG nodes, conditional routing, tool calling, WAL persistence, and distributed execution.

### 数据库
- [Database](database.md): Unified database interface — SQLite, MySQL, PostgreSQL, MongoDB, Redis connection pools, queries, and transactions.

### 消息队列
- [Message Queue](message-queue.md): In-process event bus — pub/sub messaging, middleware chains, WAL persistence, and monitoring.

### 部署运维
- [Deployment](deployment.md): Docker, systemd, multi-worker tuning, TLS/HTTPS, Prometheus, health checks, and kernel tuning.

### 系统扩展
- [Reflection](reflection.md): Compile-time struct introspection and automatic JSON serialization/deserialization.
- [Admin Dashboard](admin.md): Web-based monitoring for HTTP, AI, MQ, and database metrics.
- [Hooks](hooks.md): Lifecycle hook system — server start/stop, connection open/close, request begin/end.
- [Arena](arena.md): Bump allocator — zero-fragmentation request-scoped memory management.
- [Hot Reload](hot-reload.md): Live router swapping without restarting the server.

### Python
- [Python Bindings](python.md): Installation and API reference for the Python bindings.
