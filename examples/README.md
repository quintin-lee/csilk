# csilk Examples

This directory contains example programs demonstrating csilk features, organized by category for easier navigation. All C examples are built automatically via CMake; Python examples run independently.

## Directory Structure

```
examples/
├── basic/              # Getting started examples
├── middleware/         # Middleware and core feature demonstrations
├── database/           # Database driver examples
├── websocket/          # WebSocket and real-time communication
├── ai/                 # AI integration and workflow examples
└── advanced/           # Advanced features (TLS, hot-reload, custom drivers)
```

## Quick Start

```bash
# Build all C examples
cd build && cmake .. && make -j$(nproc)

# Run an example from any category
./build/examples/basic/example_server        # Low-level API
./build/examples/basic/example_app            # High-level app API
```

## C Examples by Category

### Basic Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [example_server.c](basic/example_server.c) | `basic/example_server.c` | `example_server` | Low-level core API: router, groups, middleware, WebSocket, SSE, multipart upload, cookies, gzip, YAML config loading |
| [example_app.c](basic/example_app.c) | `basic/example_app.c` | `example_app` | High-level `csilk_app_t` API: reflection binding, nested structs, route groups, SSE, gzip, Swagger UI |

### Middleware Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [example_sse.c](middleware/example_sse.c) | `middleware/example_sse.c` | `example_sse` | Standalone Server-Sent Events streaming via libuv timer |
| [example_admin_dashboard.c](middleware/example_admin_dashboard.c) | `middleware/example_admin_dashboard.c` | — (commented out) | Admin dashboard: real-time HTTP metrics, multi-worker stats, MQ monitoring |

### Database Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [example_db.c](database/example_db.c) | `database/example_db.c` | `example_db` | SQLite database operations via the built-in DB driver |
| [example_custom_driver.c](advanced/example_custom_driver.c) | `advanced/example_custom_driver.c` | `example_custom_driver` | Custom pluggable database driver implementation and registration |

### WebSocket Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [example_websocket.c](websocket/example_websocket.c) | `websocket/example_websocket.c` | `example_websocket` | WebSocket handshake, echo server, room-based chat broadcasting |
| [example_ws_tls_mq.c](websocket/example_ws_tls_mq.c) | `websocket/example_ws_tls_mq.c` | `example_ws_tls_mq` | Secure WebSocket (WSS) over TLS with Message Queue broadcasting |

### AI & Workflow Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [example_ai.c](ai/example_ai.c) | `ai/example_ai.c` | `example_ai` | AI unified interface: chat completions (OpenAI / Ollama) |
| [example_ai_providers.c](ai/example_ai_providers.c) | `ai/example_ai_providers.c` | `example_ai_providers` | Multi-provider AI: switching between OpenAI and Ollama at runtime |
| [example_ai_workflow.c](ai/example_ai_workflow.c) | `ai/example_ai_workflow.c` | `example_ai_workflow` | Multi-step AI Research Assistant workflow with DAG scheduling |
| [example_workflow_ui.c](ai/example_workflow_ui.c) | `ai/example_workflow_ui.c` | — (manual build) | Real-time AI workflow dashboard with live execution monitoring |

### Server Configuration Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [example_tls.c](advanced/example_tls.c) | `advanced/example_tls.c` | `example_tls` | HTTPS/TLS server with certificate configuration |
| [hot_reload_app.c](advanced/hot_reload_app.c) | `advanced/hot_reload_app.c` | — (manual build as `.so`) | Hot-reloadable app module — compiled as shared library |
| [hot_reload_launcher.c](advanced/hot_reload_launcher.c) | `advanced/hot_reload_launcher.c` | — (manual build) | Runner that loads and monitors hot-reloadable app modules |

### Advanced Examples

| Example | Path | Build Target | Demonstrates |
|---------|------|-------------|--------------|
| [advanced_server.c](advanced/advanced_server.c) | `advanced/advanced_server.c` | — (manual build) | Route groups with auth middleware, WebSocket integration |
| [config_multi.yaml](advanced/config_multi.yaml) | `advanced/config_multi.yaml` | N/A | Multi-environment configuration example |

## Python Example

| Example | Path | Demonstrates |
|---------|------|-------------|
| [workflow_agent.py](ai/workflow_agent.py) | `ai/workflow_agent.py` | AI workflow agent in Python: multi-step research using the `csilk.workflow` module |
| [agent.yaml](ai/agent.yaml) | `ai/agent.yaml` | YAML workflow definition consumed by `workflow_agent.py` |

## Running Examples

```bash
# C examples (after build)
cd build
./examples/basic/example_server          # Listen on :8080
./examples/basic/example_app             # Listen on :8080
./examples/database/example_db           # SQLite demo
./examples/middleware/example_sse        # SSE streaming on :8080
./examples/advanced/example_tls          # HTTPS on :8443 (requires cert files)
./examples/websocket/example_websocket   # WebSocket on :8080/ws
./examples/websocket/example_ws_tls_mq   # WSS + MQ on :8443

# Python example
python3 examples/ai/workflow_agent.py
```

> **Note**: TLS examples require certificate files. See [advanced/example_tls.c](advanced/example_tls.c) for details.
> Hot-reload examples require manual compilation as shared libraries — see comments in the source files.

## CMake Build Targets

All C examples except `advanced_server`, `hot_reload_app`, and `hot_reload_launcher` are built automatically when you run `make` in the build directory:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Targets will be available under examples/ subdirectories in the build directory
```
