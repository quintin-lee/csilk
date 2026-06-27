# Python Bindings User Manual

`csilk` provides high-performance, developer-friendly Python bindings using the Python standard library `ctypes` module. Developers can write high-performance web applications and AI workflows in Python while leveraging the underlying asynchronous speed of `csilk` (built on libuv, nghttp2, and OpenSSL). The shared library (`libcsilk.so`) **MUST** be discoverable via `LD_LIBRARY_PATH` or system library path. Python 3.8+ **MUST** be used (`ctypes.CDLL` with `RTLD_GLOBAL`). The Python interface **SHOULD** mirror the C API — all response methods (`string()`, `json()`, `file()`) are available with identical semantics. Asynchronous handlers **MUST NOT** block the GIL for more than 100ms.

---

## Installation

Ensure that the core `csilk` C library is compiled (e.g., in the `build/` directory or system library path). Then install the Python package:

```bash
pip install -e ./python
```

---

## Minimal Web Application

Here is a minimal web application showing route definition and running the libuv event loop:

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

## Form URL-Encoded Handling

For POST requests with `application/x-www-form-urlencoded` content type:

```python
from csilk import App, Context

app = App()

@app.post("/login")
def login(ctx: Context):
    # Parse the form body into form_fields dict
    ctx.parse_form_urlencoded()
    
    # Access form values as a dict
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

## Core API Reference

### 1. App Routing & Configuration
The `App` class is the central coordinator for route registrations, global/prefix middlewares, and server configurations:

- **HTTP Route Registration**: `@app.get(path)`, `@app.post(path)`, `@app.put(path)`, `@app.delete(path)`, etc.
- **Middleware**:
  - Global: `app.use(middleware_fn)`
  - Grouped by prefix: `app.use_group(prefix, middleware_fn)`
- **SPA Fallback**: `app.set_spa_fallback(doc_root)` (redirects missing routes to `doc_root/index.html` for single-page applications).
- **Custom 404 Handler**: `app.set_not_found_handler(handler_fn)`
- **OpenAPI Doc**: `app.enable_openapi(True)`
- **Server Stats**: `app.server_stats()` (returns active/pooled connection counts).

### 2. Context (`Context`)
The `Context` object encapsulates the request/response lifecycle:

- **Request Data**:
  - `ctx.body`: raw request body (`bytes`).
  - `ctx.query(key)`: gets query string parameters.
  - `ctx.param(key)`: gets path parameters (e.g. from `/user/:id`).
  - `ctx.get_header(key)`: retrieves a request header.
  - `ctx.get_cookie(key)`: retrieves request cookies.
- **Response Manipulation**:
  - `ctx.string(status, text)`: writes a string response.
  - `ctx.json(status, dict)`: writes a JSON response.
  - `ctx.set_header(key, val)`: sets response headers.
  - `ctx.set_cookie(...)`: sets response cookies.
  - `ctx.response_body`: gets or sets the response body.
  - `ctx.abort()`: stops middleware chain execution.
  - `ctx.next()`: calls the next middleware.
- **Server-Sent Events (SSE)**:
  - `ctx.sse_init()`: upgrades request to event-stream.
  - `ctx.is_sse`: property indicating if the context is an active SSE stream.
  - `ctx.sse_send(event, data)`: sends an event message.
  - `ctx.sse_close()`: closes the event stream.
- **WebSocket Handshake**:
  - `ctx.ws_handshake()`: upgrades to WebSocket.
  - `ctx.set_on_ws_message(cb)`: registers message event handler.
  - `ctx.ws_send(payload)`: sends a WebSocket frame.
  - `ctx.ws_close()`: closes the socket.
- **Multipart Form parsing**:
  - `ctx.multipart_parse(handler_fn)`: parses multipart file uploads synchronously. Fires `handler_fn(part_dict)` for each form field or file.
  - `ctx.parse_form_urlencoded()`: parses URL-encoded form bodies (POST with `application/x-www-form-urlencoded`). Must be called before accessing `ctx.form_fields`.
  - `ctx.form_fields`: property returning a dict of all parsed form fields after calling `parse_form_urlencoded()`.
  - `ctx.queries`: property returning a dict of all query string parameters.
  - `ctx.headers`: property returning a case-insensitive dict of all request headers.

---

## AI Workflow Engine

The `Workflow` class enables constructing, executing, and visualizing complex agentic workflows:

```python
from csilk.workflow import Workflow

wf = Workflow("agentic_pipeline")

# Add standard Python handlers
def step1(ctx, inp):
    return ctx.data_new_string(inp.value.upper())

n1 = wf.add("node1", step1)
n1.set_entry(True)

# Add built-in AI configuration nodes
n2 = wf.add_ai(
    "node2",
    model="gpt-4",
    system_msg="You are a helpful assistant",
    prompt="Translate this text to French: {{node1.value}}"
)

# Connect nodes
n1.bind(n2)

# Run workflow
exec_id = wf.run("hello world", callback=lambda res: print("Result:", res.value))
```

### Advanced Workflow Orchestration:
- **Join Policies**: `node.set_join_policy("AND")` or `node.set_join_policy("OR")`.
- **Timeouts & Retries**: `node.set_timeout(5000)` and `node.set_retry(max_retries=3, delay_ms=1000)`.
- **Mermaid Exporter**: `wf.to_mermaid()` outputs visual Mermaid graph markup of the pipeline.
- **Interactive Pause & WAL Resume**:
  - Mark a node: `node.set_interactive(True)`.
  - Enable persistence: `wf.set_persistence(wal_directory)`.
  - Resume execution: `wf.resume(exec_id)`.
  - Approve & supply human feedback: `wf.signal_continue(exec_id, "feedback_data", callback)`.
- **Dynamic Tool Discovery**:
  - Register tools: `wf.register_tool(name, desc, schema, fn)`.
  - Enable dynamic tool callbacks (MCP-like): `wf.set_tool_discovery(discovery_callback_fn)`.

---

## Database Pools (`DBPool`)

Thread-safe database pooling and SQL query wrapper:

```python
from csilk import DBPool

# Initialize SQLite database pool
db = DBPool("sqlite", "file::memory:?cache=shared")

# Execute queries
db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)")
db.execute("INSERT INTO users (name) VALUES (?)", ["Alice"])

# Query rows
rows = db.query("SELECT * FROM users")
for row in rows:
    print(row["name"])

# Observing metrics
stats = DBPool.get_stats()
print("Queries Executed:", stats["queries_total"])

db.free()
```

Supported drivers: `sqlite`, `mysql`, `postgres`, `mongodb`, `redis`.

---

## Permissions & RBAC (`Perm`)

Role-based access control with the built-in in-memory RBAC driver:

```python
from csilk import Perm, Context

# Initialise the permission subsystem
Perm.init()
Perm.simple_init()

# Grant permissions: allow "admin" role to "write" on "orders:*"
Perm.simple_allow("admin", "write", "orders:*")
Perm.simple_allow("admin", "read", "orders:*")
Perm.simple_allow("editor", "read", "orders:*")

# Inside a middleware or handler
def admin_only(ctx: Context):
    # Check permission — aborts with 403 if denied
    Perm.require(ctx, "write", "orders:123")

# Or use declarative route metadata with auto middleware:
Perm.set_default("simple")
# Perm.auto_middleware reads perm_required + perm_resource
# from route metadata registered via App
```

The permission system supports pluggable backends. The built-in `"simple"` driver stores `(role, permission, resource)` triples in memory.

---

## Message Queue (`MQ`)

Asynchronous internal event bus for publish/subscribe messaging:

```python
from csilk import MQ, MqContext

# Get the server's MQ instance from an App
mq = app.get_mq()

# Subscribe to a topic (supports fnmatch glob patterns)
@mq.subscribe("order.*")
def handle_order(ctx: MqContext, payload: str):
    print(f"Received on {ctx.topic}: {payload}")

# Publish a message asynchronously
mq.publish("order.created", "Order #123 placed")

# Configure WAL persistence on MQ creation
mq_with_wal = MQ(wal_dir="/var/log/csilk/mq")
```

**Topic Patterns**: Supports `*` (match any), `?` (single char), `[seq]` (character class) via `fnmatch`.

---

## Vector Database (`VectorDB`)

Interface for vector similarity search backends:

```python
from csilk import VectorDB

# Connect to Qdrant
vdb = VectorDB("qdrant", endpoint="http://localhost:6333", api_key="")

# Upsert vectors
vdb.upsert("my_collection", [
    {"id": "doc1", "vector": [0.1, 0.2, ...], "payload": {"title": "Doc 1"}},
])

# Search by vector
results = vdb.search("my_collection", query_vector=[0.1, 0.2, ...], limit=5)
for r in results:
    print(r.id, r.score, r.payload)
```

---

## Cryptography & Security (`Crypto`)

Helper class exposing OpenSSL-backed cryptographic tools:

```python
from csilk import Crypto

# Cryptographically secure random bytes
nonce = Crypto.random_bytes(16)

# Generate RFC 4122 UUIDs
uuid_str = Crypto.generate_uuid()

# CSRF token generation (hex-encoded 32-character)
token = Crypto.generate_csrf_token()
```

---

## Further Reading

Deep-dive architectural documentation for the underlying C module implementations can be found in [`docs/module-design/`](../module-design/index.md):

| Core | Data & AI | Security |
|------|-----------|----------|
| [Server Core](../module-design/server.md) | [Data Layer](../module-design/data.md) | [RBAC/Permissions](../module-design/security.md) |
| [App Layer](../module-design/app.md) | [AI Engine](../module-design/ai.md) | [Crypto](../module-design/crypto.md) |
| [Router](../module-design/router.md) | [Workflow](../module-design/workflow.md) | [JWT/CSRF/CORS](../module-design/security.md) |
| [Context & Arena](../module-design/context.md) | [Messaging/MQ](../module-design/messaging.md) | [WebSocket/SSE](../module-design/protocols.md) |
| [Middleware Models](../module-design/middleware.md) | [Drivers (DB/AI/Cipher)](../module-design/drivers.md) | [Metrics/Prometheus](../module-design/metrics.md) |
