# Model Context Protocol (MCP) & Declarative Workflow DSL Design Specification

## Overview

This specification defines the architecture, components, API contracts, and implementation details for adding native **Model Context Protocol (MCP)** Server/Client capabilities, a **Declarative JSON/YAML Workflow DSL**, and a **WebSocket Live Debugging & Hot-Reloading Engine** to the `csilk` (server-c) web framework.

The goal is to elevate `csilk` into a first-class AI Developer Ecosystem foundation, allowing LLM agents, external IDEs (Claude Desktop, Cursor, Copilot), and visual workflow editors to seamlessly inspect, execute, debug, and dynamically update AI workflows written in C.

---

## 1. System Architecture & Module Boundaries

### 1.1 Directory Structure

```
include/csilk/
  ├── protocols/
  │   └── mcp.h               # Public MCP Server & Client interfaces
  └── app/
      └── workflow_dsl.h      # Public Workflow DSL & Hot-Reloading Manager interfaces

src/
  ├── protocols/
  │   └── mcp/
  │       ├── mcp_jsonrpc.c   # JSON-RPC 2.0 message parser and builder
  │       ├── mcp_server.c    # MCP Server implementation (Stdio & SSE/HTTP Transports)
  │       ├── mcp_client.c    # MCP Client implementation (remote MCP server integration)
  │       └── mcp_internal.h  # Internal structures, frames, and transport handles
  └── workflow/
      ├── workflow_dsl.c      # JSON/YAML DSL parser, validator, and exporter
      ├── workflow_manager.c  # Atomic hot-swapping & versioned workflow registry
      └── workflow_debug.c    # WebSocket live debugging, step control, and tracing dispatcher
```

### 1.2 Architectural Constraints & Principles

1. **Zero New External Dependencies**: Built entirely using existing internal components (`cJSON`, `csilk_arena_t`, `http1_response.c`, `sse.c`, `websocket.c`, `csilk_mutex_t`).
2. **Memory Efficiency**: JSON-RPC frames, DSL AST trees, and execution context variables are allocated within thread-local `csilk_arena_t` instances for single-pass zero-fragmentation cleanup.
3. **Non-Blocking I/O**: MCP Stdio transport leverages asynchronous `uv_pipe_t` / `uring_fs` I/O. SSE, HTTP, and WebSocket debugging endpoints integrate directly into the `csilk` multi-worker event loop.

---

## 2. Model Context Protocol (MCP) Engine

### 2.1 JSON-RPC 2.0 Layer (`mcp_jsonrpc.c`)

Implements RFC 4627 compliant JSON-RPC 2.0 framing for MCP specification `2024-11-05`:

```c
typedef struct {
    char        jsonrpc[8];  /* "2.0" */
    cJSON      *id;          /* Number, String, or NULL for Notifications */
    char       *method;      /* "initialize", "tools/list", "tools/call", etc. */
    cJSON      *params;
    cJSON      *result;
    cJSON      *error;       /* { "code": int, "message": string, "data": optional } */
} csilk_mcp_msg_t;

csilk_mcp_msg_t* csilk_mcp_msg_parse(const char* buf, size_t len, csilk_arena_t* arena);
char*            csilk_mcp_msg_serialize(const csilk_mcp_msg_t* msg, csilk_arena_t* arena);
csilk_mcp_msg_t* csilk_mcp_msg_create_error(cJSON* id, int code, const char* message);
```

### 2.2 MCP Server (`mcp_server.c`)

Exposes `csilk` tools and AI agent workflows to external LLMs and IDEs.

* **Capability Negotiation**: Responds to `initialize` with server capabilities (`tools`, `prompts`, `resources`).
* **Tool Discovery (`tools/list`)**: Iterates through registered `csilk_wf_tool_t` array and serializes tool names, descriptions, and JSON Schema input parameter specifications.
* **Tool Invocation (`tools/call`)**: Maps JSON-RPC invocation calls to internal `csilk_wf_tool_cb` functions or AI DAG nodes, executing the tool within an isolated context arena and returning formatted `content` objects.
* **Prompt Registry (`prompts/list`, `prompts/get`)**: Exposes workflow prompt templates for IDE context injection.

#### Transports:
- **Stdio Transport**: Non-blocking asynchronous stdin/stdout pipe for CLI integrations (e.g., Claude Desktop config: `"command": "./bin/my_csilk_app", "args": ["--mcp-stdio"]`).
- **SSE Transport**: Mounted on `csilk_app_t` at `/mcp/sse` (GET for event stream) and `/mcp/message` (POST for JSON-RPC messages).

### 2.3 MCP Client (`mcp_client.c`)

Allows `csilk` workflows to consume third-party external MCP servers.

- Connects to external Stdio processes or remote SSE endpoints.
- Issues `initialize` and `tools/list` requests to discover available remote tools.
- Automatically wraps remote tools into native `csilk_wf_node_t` handlers (`type: "mcp_tool"`). When a workflow reaches an MCP tool node, `mcp_client` dispatches a `tools/call` request over the channel and waits for the async result.

---

## 3. Declarative Workflow DSL Specification (`workflow_dsl.c`)

### 3.1 Schema Specification

Workflows can be fully defined in JSON or YAML format:

```json
{
  "name": "support_pipeline",
  "version": "1.0.0",
  "description": "Automated customer support routing pipeline",
  "budget": {
    "max_tokens": 8192,
    "timeout_ms": 60000
  },
  "persistence": {
    "enabled": true,
    "wal_dir": "./wals/support_pipeline"
  },
  "nodes": [
    {
      "id": "intent_node",
      "type": "ai_chat",
      "config": {
        "model": "gpt-4o-mini",
        "system_prompt": "Classify user intent into tech or billing."
      }
    },
    {
      "id": "kb_search",
      "type": "vector_search",
      "config": {
        "collection": "support_docs",
        "top_k": 3
      },
      "depends_on": ["intent_node"]
    },
    {
      "id": "github_action",
      "type": "mcp_tool",
      "config": {
        "server_name": "github_mcp",
        "tool_name": "create_issue"
      },
      "depends_on": ["kb_search"]
    }
  ]
}
```

### 3.2 Supported Node Types
- `ai_chat`: LLM Chat node with system prompt and model configuration.
- `vector_search`: Vector Database RAG search node.
- `mcp_tool`: Local or remote MCP tool invocation node.
- `agent_react` / `agent_reflexion` / `agent_hitl` / `agent_worker`: Autonomous agent nodes.
- `router_switch`: Conditional expression branching node.

### 3.3 Parser & Validation Pipeline
1. **Syntax Parsing**: Parses JSON/YAML string into AST.
2. **DAG Validation**: Detects circular dependencies via Depth-First Search (DFS) topological sorting.
3. **Node Factory**: Constructs and wires `csilk_wf_t` and `csilk_wf_node_t` instances.
4. **Serialization**: `csilk_wf_to_json()` exports runtime `csilk_wf_t` instances back into JSON DSL format.

---

## 4. Hot-Reloading & Live WebSocket Debugging Engine

### 4.1 Atomic Workflow Manager (`workflow_manager.c`)

Provides thread-safe, zero-downtime workflow replacement in production:

```c
typedef struct {
    char            name[128];
    uint32_t        version;
    csilk_wf_t     *active_wf;
    csilk_mutex_t   mutex;
} csilk_wf_entry_t;

typedef struct {
    csilk_wf_entry_t entries[64];
    size_t           count;
    csilk_rwlock_t   rwlock;
} csilk_wf_manager_t;
```

#### Hot-Swapping Guarantee:
- On POST `/api/v1/workflows/reload`, the new DSL is parsed and compiled into a candidate `csilk_wf_t`.
- On successful validation, `active_wf` is updated atomically using an atomic pointer swap.
- Active execution contexts retain reference counts (`refcount`) on their initial `csilk_wf_t` version, continuing unaffected until completion. New executions automatically pick up the updated version.

### 4.2 WebSocket Live Debugger (`workflow_debug.c`)

Mounted on `/api/v1/workflows/:name/debug` for real-time visual UI integration:

- **Server-to-Client Events**: `node_start`, `node_complete`, `mcp_tool_call`, `hitl_waiting`, `error`.
- **Client-to-Server Commands**: `pause`, `resume`, `step`, `set_breakpoint`, `inject_data`.

---

## 5. Public API Contracts

### 5.1 `include/csilk/protocols/mcp.h`

```c
#ifndef CSILK_MCP_H
#define CSILK_MCP_H

#include "csilk/csilk.h"
#include "csilk/app/workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_mcp_server_s csilk_mcp_server_t;
typedef struct csilk_mcp_client_s csilk_mcp_client_t;

/* MCP Server Management */
csilk_mcp_server_t* csilk_mcp_server_new(const char* name, const char* version);
void                csilk_mcp_server_free(csilk_mcp_server_t* server);
int                 csilk_mcp_server_register_tool(csilk_mcp_server_t* server, csilk_wf_tool_t* tool);
int                 csilk_mcp_server_register_workflow(csilk_mcp_server_t* server, csilk_wf_t* wf);
int                 csilk_mcp_server_start_stdio(csilk_mcp_server_t* server);
int                 csilk_mcp_server_bind_app(csilk_mcp_server_t* server, csilk_app_t* app, const char* route_prefix);

/* MCP Client Management */
csilk_mcp_client_t* csilk_mcp_client_connect_stdio(const char* command, char* const argv[]);
csilk_mcp_client_t* csilk_mcp_client_connect_sse(const char* sse_url);
void                csilk_mcp_client_free(csilk_mcp_client_t* client);
int                 csilk_mcp_client_import_tools(csilk_mcp_client_t* client, csilk_wf_t* wf);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_MCP_H */
```

### 5.2 `include/csilk/app/workflow_dsl.h`

```c
#ifndef CSILK_WORKFLOW_DSL_H
#define CSILK_WORKFLOW_DSL_H

#include "csilk/app/workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_wf_manager_s csilk_wf_manager_t;

/* DSL Parsing & Export */
csilk_wf_t* csilk_wf_from_json(const char* json_str, char* err_buf, size_t err_len);
csilk_wf_t* csilk_wf_from_file(const char* filepath, char* err_buf, size_t err_len);
char*       csilk_wf_to_json(csilk_wf_t* wf);

/* Workflow Manager & Hot Reloading */
csilk_wf_manager_t* csilk_wf_manager_new(void);
void                csilk_wf_manager_free(csilk_wf_manager_t* mgr);
int                 csilk_wf_manager_register(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* wf);
int                 csilk_wf_manager_reload(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* new_wf);
csilk_wf_t*         csilk_wf_manager_get(csilk_wf_manager_t* mgr, const char* name);
int                 csilk_wf_manager_enable_debug_server(csilk_wf_manager_t* mgr, csilk_app_t* app, const char* route_path);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_WORKFLOW_DSL_H */
```

---

## 6. Verification & Test Plan

1. **`test_mcp_jsonrpc.c`**: Unit test JSON-RPC 2.0 parsing, serialization, and error frame creation.
2. **`test_mcp_server_client.c`**: End-to-end integration test connecting MCP Client and MCP Server via Stdio pipe and Mock SSE endpoints (`tools/list`, `tools/call`).
3. **`test_workflow_dsl.c`**: Test JSON/YAML DSL parsing, DAG validation (cycle detection), node factory wiring, and `csilk_wf_to_json()` export.
4. **`test_workflow_hotreload.c`**: Multi-threaded stress test issuing continuous workflow executions while triggering atomic workflow reloads, verifying zero in-flight context failures.
