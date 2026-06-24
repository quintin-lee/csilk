# csilk Python Bindings

Python bindings for the `csilk` C web framework and AI workflow orchestrator.

## Overview

The `csilk` Python package provides high-level, developer-friendly wrappers around the csilk C library. It leverages Python's `ctypes` module to interface with the native library, exposing all core framework features including routing, middleware, sessions, SSE, database pooling, and AI workflow pipelines.

## Prerequisites

Before installing the Python package, you must compile the underlying C library:

```bash
# From the repository root
mkdir build && cd build
cmake .. -DCSIL treating compiler errors as build failures -DCSILK_BUILD_SHARED=ON
make
```

Ensure `libcsilk.so` (Linux) or `libcsilk.dylib` (macOS) is in your library path or the root directory.

## Installation

To install the package in editable development mode:

```bash
cd python
pip install -e .
```

> [!NOTE]
> The underlying C library (`libcsilk.so` or `libcsilk.dylib`) must be compiled and available in your library search path or in the root directory as resolved by the ctypes loader.

## Quick Start

```python
from csilk import App, Context

app = App()

@app.get("/hello")
def hello(ctx: Context):
    ctx.string(200, "Hello World from Python!")

if __name__ == "__main__":
    app.run(8080)
```

## API Overview

### Core Classes

- **`App`** — Application entry point. Manages routing, middleware, server lifecycle, and WebSocket contexts.
- **`Group`** — Route grouping with prefix path support.
- **`Context`** — Request/response lifecycle wrapper. Provides access to headers, body, JSON, query params, cookies, sessions, SSE, and more.

### Middleware Modules

- **`recovery_middleware`** — Automatic panic recovery (returns 500 on unhandled exceptions).
- **`logger_middleware`** — Request logging with latency.
- **`cors`** — CORS header handling.
- **`jwt_middleware`** — JWT authentication.
- **`csrf_middleware`** — CSRF token validation.
- **`waf_middleware`** — Web Application Firewall.
- **`rate_limit`** — Rate limiting by IP.
- **`gzip_middleware`** — Gzip response compression.
- **`request_id_middleware`** — Injects `X-Request-Id` for tracing.

### Data & AI

- **`DBPool`** — Database connection pool management (SQLite, MySQL, PostgreSQL, MongoDB, Redis).
- **`AI`** — Unified AI interface for Chat, Embeddings, and Tool Calling (OpenAI, Ollama).
- **`VectorDB`** — Vector database interface (Qdrant, Milvus).
- **`Perm`** — Permission system for role-based access control.
- **`Crypto`** — Cryptographic utilities (hashing, UUID generation).
- **`Workflow`** — AI workflow orchestration engine with nodes, context, and data flow.
- **`MQ`** — Message queue for asynchronous event bus communication.

## Running Tests

To run the Python unit test suite:

```bash
python3 -m unittest discover -s tests
```

## Resources

- [User Manual](../docs/user-manual/python.md) — Detailed Python API reference and workflow guides.
- [Main README](../README.md) — Framework overview and C API documentation.
