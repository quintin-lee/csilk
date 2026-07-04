# Python 绑定用户手册

`csilk` 提供了使用 Python 标准库 `ctypes` 模块的高性能、开发者友好的 Python 绑定。开发人员可以在 Python 中编写高性能 Web 应用程序和 AI 工作流，同时利用底层 `csilk`（基于 libuv、nghttp2 和 OpenSSL）的异步速度。共享库 (`libcsilk.so`) **MUST** 通过 `LD_LIBRARY_PATH` 或系统库路径可见。Python 3.8+ **MUST** 使用（`ctypes.CDLL` 带 `RTLD_GLOBAL`）。Python 接口 **SHOULD** 镜像 C API — 所有响应方法（`string()`、`json()`、`file()`）都具有相同语义。异步处理器 **MUST NOT** 阻塞 GIL 超过 100ms。

---

## 安装

确保核心 `csilk` C 库已编译（例如，在 `build/` 目录或系统库路径中）。然后安装 Python 包：

```bash
pip install -e ./python
```

---

## 最小 Web 应用程序

以下是一个最小的 Web 应用程序，展示了路由定义和运行 libuv 事件循环：

```python
from csilk import App, Context

app = App()

@app.get("/hello")
def hello(ctx: Context):
    ctx.string(200, "Hello World from Python!")

if __name__ == "__main__":
    app.run(8080)
```

---

## 表单 URL 编码处理

对于具有 `application/x-www-form-urlencoded` 内容类型的 POST 请求：

```python
from csilk import App, Context

app = App()

@app.post("/login")
def login(ctx: Context):
    # 将表单 body 解析为 form_fields 字典
    ctx.parse_form_urlencoded()
    
    # 将表单值作为字典访问
    username = ctx.form_fields.get("username")
    password = ctx.form_fields.get("password")
    
    if username and password:
        ctx.json(200, {"message": f"Login as {username}"})
    else:
        ctx.json(400, {"error": "Missing credentials"})

if __name__ == "__main__":
    app.run(8080)
```

---

## 核心 API 参考

### 1. App 路由和配置

`App` 类是路由注册、全局/前缀中间件和服务器配置的中央协调器：

- **HTTP 路由注册**: `@app.get(path)`, `@app.post(path)`, `@app.put(path)`, `@app.delete(path)` 等。
- **中间件**:
  - 全局: `app.use(middleware_fn)`
  - 按前缀分组: `app.use_group(prefix, middleware_fn)`
- **SPA 回退**: `app.set_spa_fallback(doc_root)`（将缺失的路由重定向到 `doc_root/index.html` 以支持单页应用程序）。
- **自定义 404 处理器**: `app.set_not_found_handler(handler_fn)`
- **OpenAPI 文档**: `app.enable_openapi(True)`
- **服务器统计**: `app.server_stats()`（返回活跃/池化连接数）。

### 2. Context (`Context`)

`Context` 对象封装了请求/响应生命周期：

- **请求数据**:
  - `ctx.body`: 原始请求体（`bytes`）。
  - `ctx.query(key)`: 获取查询字符串参数。
  - `ctx.param(key)`: 获取路径参数（例如来自 `/user/:id`）。
  - `ctx.get_header(key)`: 检索请求头。
  - `ctx.get_cookie(key)`: 检索请求 cookie。
- **响应操作**:
  - `ctx.string(status, text)`: 写入字符串响应。
  - `ctx.json(status, dict)`: 写入 JSON 响应。
  - `ctx.set_header(key, val)`: 设置响应头。
  - `ctx.set_cookie(...)`: 设置响应 cookie。
  - `ctx.response_body`: 获取或设置响应体。
  - `ctx.abort()`: 停止中间件链执行。
  - `ctx.next()`: 调用下一个中间件。
- **Server-Sent Events (SSE)**:
  - `ctx.sse_init()`: 升级请求到事件流。
  - `ctx.is_sse`: 指示上下文是否为活动 SSE 流的属性。
  - `ctx.sse_send(event, data)`: 发送事件消息。
  - `ctx.sse_close()`: 关闭事件流。
- **WebSocket 握手**:
  - `ctx.ws_handshake()`: 升级到 WebSocket。
  - `ctx.set_on_ws_message(cb)`: 注册消息事件处理器。
  - `ctx.ws_send(payload)`: 发送 WebSocket 帧。
  - `ctx.ws_close()`: 关闭套接字。
- **多部分表单解析**:
  - `ctx.multipart_parse(handler_fn)`: 同步解析多部分文件上传。为每个表单字段或文件触发 `handler_fn(part_dict)`。
  - `ctx.parse_form_urlencoded()`: 解析 URL 编码的表单 body。必须在访问 `ctx.form_fields` 之前调用。
  - `ctx.form_fields`: 属性，在调用 `parse_form_urlencoded()` 后返回所有解析表单字段的字典。
  - `ctx.queries`: 属性，返回所有查询字符串参数的字典。
  - `ctx.headers`: 属性，返回所有请求头的不区分大小写字典。

---

## AI 工作流引擎

`Workflow` 类支持构建、执行和可视化复杂的代理工作流：

```python
from csilk.workflow import Workflow

wf = Workflow("agentic_pipeline")

# 添加标准 Python 处理器
def step1(ctx, inp):
    return ctx.data_new_string(inp.value.upper())

n1 = wf.add("node1", step1)
n1.set_entry(True)

# 添加内置 AI 配置节点
n2 = wf.add_ai(
    "node2",
    model="gpt-4",
    system_msg="You are a helpful assistant",
    prompt="Translate this text to French: {{node1.value}}"
)

# 连接节点
n1.bind(n2)

# 运行工作流
exec_id = wf.run("hello world", callback=lambda res: print("Result:", res.value))
```

### 高级工作流编排:
- **连接策略**: `node.set_join_policy("AND")` 或 `node.set_join_policy("OR")`。
- **超时和重试**: `node.set_timeout(5000)` 和 `node.set_retry(max_retries=3, delay_ms=1000)`。
- **Mermaid 导出器**: `wf.to_mermaid()` 输出管道的可视 Mermaid 图标记。
- **交互式暂停和 WAL 恢复**:
  - 标记节点: `node.set_interactive(True)`。
  - 启用持久化: `wf.set_persistence(wal_directory)`。
  - 恢复执行: `wf.resume(exec_id)`。
  - 批准并提供人工反馈: `wf.signal_continue(exec_id, "feedback_data", callback)`。
- **动态工具发现**:
  - 注册工具: `wf.register_tool(name, desc, schema, fn)`。
  - 启用动态工具回调（类似 MCP）: `wf.set_tool_discovery(discovery_callback_fn)`。

---

## 数据库池 (`DBPool`)

线程安全的数据库池和 SQL 查询包装器：

```python
from csilk import DBPool

# 初始化 SQLite 数据库池
db = DBPool("sqlite", "file::memory:?cache=shared")

# 执行查询
db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)")
db.execute("INSERT INTO users (name) VALUES (?)", ["Alice"])

# 查询行
rows = db.query("SELECT * FROM users")
for row in rows:
    print(row["name"])

# 观察指标
stats = DBPool.get_stats()
print("Queries Executed:", stats["queries_total"])

db.free()
```

支持的驱动: `sqlite`, `mysql`, `postgres`, `mongodb`, `redis`。

---

## 权限和 RBAC (`Perm`)

使用内置内存 RBAC 驱动进行基于角色的访问控制：

```python
from csilk import Perm, Context

# 初始化权限子系统
Perm.init()
Perm.simple_init()

# 授予权限: 允许 "admin" 角色在 "orders:*" 上 "write"
Perm.simple_allow("admin", "write", "orders:*")
Perm.simple_allow("admin", "read", "orders:*")
Perm.simple_allow("editor", "read", "orders:*")

# 在中间件或处理器内
def admin_only(ctx: Context):
    # 检查权限 — 如果拒绝则中止并返回 403
    Perm.require(ctx, "write", "orders:123")

# 或使用声明式路由元数据与自动中间件:
Perm.set_default("simple")
# Perm.auto_middleware 从路由元数据读取 perm_required + perm_resource
# 这些在通过 App 注册
```

权限系统支持可插拔后端。内置的 `"simple"` 驱动在内存中存储 `(role, permission, resource)` 三元组。

---

## 消息队列 (`MQ`)

用于发布/订阅消息传递的异步内部事件总线：

```python
from csilk import MQ, MqContext

# 从 App 获取服务器的 MQ 实例
mq = app.get_mq()

# 订阅主题（支持 fnmatch 通配模式）
@mq.subscribe("order.*")
def handle_order(ctx: MqContext, payload: str):
    print(f"Received on {ctx.topic}: {payload}")

# 异步发布消息
mq.publish("order.created", "Order #123 placed")

# 在 MQ 创建时配置 WAL 持久化
mq_with_wal = MQ(wal_dir="/var/log/csilk/mq")
```

**主题模式**: 支持 `*` (匹配任意)、`?` (单字符)、`[seq]` (字符类) 通过 `fnmatch`。

---

## 向量数据库 (`VectorDB`)

向量相似度搜索后端的接口：

```python
from csilk import VectorDB

# 连接到 Qdrant
vdb = VectorDB("qdrant", endpoint="http://localhost:6333", api_key="")

# 插入向量
vdb.upsert("my_collection", [
    {"id": "doc1", "vector": [0.1, 0.2, ...], "payload": {"title": "Doc 1"}},
])

# 通过向量搜索
results = vdb.search("my_collection", query_vector=[0.1, 0.2, ...], limit=5)
for r in results:
    print(r.id, r.score, r.payload)
```

---

## 密码学和安全 (`Crypto`)

暴露 OpenSSL 支持的加密工具的帮助类：

```python
from csilk import Crypto

# 加密安全的随机字节
nonce = Crypto.random_bytes(16)

# 生成 RFC 4122 UUID
uuid_str = Crypto.generate_uuid()

# CSRF 令牌生成（十六进制编码的 32 字符）
token = Crypto.generate_csrf_token()
```

---

## 进一步阅读

关于底层 C 模块实现的深度架构文档可在 [`docs/module-design/`](../module-design/index.md) 中找到：

| 核心 | 数据与 AI | 安全 |
|------|-----------|------|
| [Server Core](../module-design/server.md) | [Data Layer](../module-design/data.md) | [RBAC/权限](../module-design/security.md) |
| [App Layer](../module-design/app.md) | [AI Engine](../module-design/ai.md) | [Crypto](../module-design/crypto.md) |
| [Router](../module-design/router.md) | [Workflow](../module-design/workflow.md) | [JWT/CSRF/CORS](../module-design/security.md) |
| [Context & Arena](../module-design/context.md) | [Messaging/MQ](../module-design/messaging.md) | [WebSocket/SSE](../module-design/protocols.md) |
| [Middleware Models](../module-design/middleware.md) | [Drivers (DB/AI/Cipher)](../module-design/drivers.md) | [Metrics/Prometheus](../module-design/metrics.md) |