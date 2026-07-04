# csilk Examples

This directory contains example programs demonstrating csilk features. All C examples are built automatically via CMake; Python examples run independently.

## Quick Start

```bash
# Build all C examples
cd build && cmake .. && make -j$(nproc)

# Run an example
./build/example_server        # Low-level API
./build/example_app            # High-level app API
```

## C Examples

### Core API

| Example | Build Target | Demonstrates |
|---------|-------------|--------------|
| [example_server.c](example_server.c) | `example_server` | Low-level core API: router, groups, middleware, WebSocket, SSE, multipart upload, cookies, gzip, YAML config loading |
| [example_app.c](example_app.c) | `example_app` | High-level `csilk_app_t` API: reflection binding, nested structs, route groups, SSE, gzip, Swagger UI |
| [advanced_server.c](advanced_server.c) | — (manual build) | Route groups with auth middleware, WebSocket integration |

### Server Configuration

| Example | Build Target | Demonstrates |
|---------|-------------|--------------|
| [example_tls.c](example_tls.c) | `example_tls` | HTTPS/TLS server with certificate configuration |
| [example_sse.c](example_sse.c) | `example_sse` | Standalone Server-Sent Events streaming via libuv timer |
| [example_websocket.c](example_websocket.c) | `example_websocket` | WebSocket handshake, echo server, room-based chat broadcasting |
| [example_ws_tls_mq.c](example_ws_tls_mq.c) | `example_ws_tls_mq` | Secure WebSocket (WSS) over TLS with Message Queue broadcasting |
| [hot_reload_app.c](hot_reload_app.c) | — (manual build as `.so`) | Hot-reloadable app module — compiled as shared library |
| [hot_reload_launcher.c](hot_reload_launcher.c) | — (manual build) | Runner that loads and monitors hot-reloadable app modules |

### Admin & Observability

| Example | Build Target | Demonstrates |
|---------|-------------|--------------|
| [example_admin_dashboard.c](example_admin_dashboard.c) | — (commented out) | Admin dashboard: real-time HTTP metrics, multi-worker stats, MQ monitoring |

### AI & Workflow

| Example | Build Target | Demonstrates |
|---------|-------------|--------------|
| [example_ai.c](example_ai.c) | `example_ai` | AI unified interface: chat completions (OpenAI / Ollama) |
| [example_ai_providers.c](example_ai_providers.c) | `example_ai_providers` | Multi-provider AI: switching between OpenAI and Ollama at runtime |
| [example_ai_workflow.c](example_ai_workflow.c) | `example_ai_workflow` | Multi-step AI Research Assistant workflow with DAG scheduling |
| [example_workflow_ui.c](example_workflow_ui.c) | `example_workflow_ui` | Real-time AI workflow dashboard with live execution monitoring |

### Database & Drivers

| Example | Build Target | Demonstrates |
|---------|-------------|--------------|
| [example_db.c](example_db.c) | `example_db` | SQLite database operations via the built-in DB driver |
| [example_custom_driver.c](example_custom_driver.c) | `example_custom_driver` | Custom pluggable database driver implementation and registration |

## Python Example

| Example | Demonstrates |
|---------|-------------|
| [workflow_agent.py](workflow_agent.py) | AI workflow agent in Python: multi-step research using the `csilk.workflow` module |
| [agent.yaml](agent.yaml) | YAML workflow definition consumed by `workflow_agent.py` |

## Running

```bash
# C examples (after build)
cd build
./example_server          # Listen on :8080
./example_app             # Listen on :8080
./example_db              # SQLite demo
./example_sse             # SSE streaming on :8080
./example_tls             # HTTPS on :8443 (requires cert files)
./example_websocket       # WebSocket on :8080/ws
./example_ws_tls_mq       # WSS + MQ on :8443

# Python example
python3 examples/workflow_agent.py
```

> **Note**: TLS examples require certificate files. See [example_tls.c](example_tls.c) for details.
> Hot-reload examples require manual compilation as shared libraries — see comments in the source files.

## CMake Build Targets

All C examples except `advanced_server`, `hot_reload_app`, and `hot_reload_launcher` are built automatically when you run `make` in the build directory:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Targets: example_server, example_app, example_ai, example_db, ...
```
