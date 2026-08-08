# Model Context Protocol (MCP) & Declarative Workflow DSL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement native MCP Server & Client capabilities, a Declarative JSON/YAML Workflow DSL, and an Atomic Hot-Reloading & WebSocket Live Debugging engine in `csilk` (server-c).

**Architecture:** Add MCP protocol handlers under `src/protocols/mcp/`, DSL parser and hot-reloading manager under `src/workflow/`, public headers in `include/csilk/protocols/mcp.h` and `include/csilk/app/workflow_dsl.h`. Zero external dependencies added.

**Tech Stack:** C23, CMake, libuv / io_uring, cJSON, clang-format, clang-tidy

**Spec:** `docs/superpowers/specs/2026-08-08-mcp-workflow-dsl-design.md`

---

## File Structure

### New Files to Create

| File | Responsibility |
|------|---------------|
| `include/csilk/protocols/mcp.h` | Public API header for MCP Server & Client |
| `include/csilk/app/workflow_dsl.h` | Public API header for Workflow DSL & Manager |
| `src/protocols/mcp/mcp_internal.h` | Internal JSON-RPC 2.0 frames & transport handles |
| `src/protocols/mcp/mcp_jsonrpc.c` | JSON-RPC 2.0 frame parsing, serialization, and error creation |
| `src/protocols/mcp/mcp_server.c` | MCP Server implementation (Stdio & SSE Transports) |
| `src/protocols/mcp/mcp_client.c` | MCP Client implementation & remote tool auto-import |
| `src/workflow/workflow_dsl.c` | Declarative JSON/YAML DSL parser, validator, and exporter |
| `src/workflow/workflow_manager.c` | Reference-counted atomic hot-swapping workflow manager |
| `src/workflow/workflow_debug.c` | WebSocket live step debugger and tracing dispatcher |
| `tests/test_mcp_jsonrpc.c` | Unit tests for JSON-RPC 2.0 messaging |
| `tests/test_mcp_server_client.c` | End-to-end integration tests for MCP Server/Client |
| `tests/test_workflow_dsl.c` | Unit tests for JSON/YAML DSL parsing and DAG validation |
| `tests/test_workflow_hotreload.c` | Multi-threaded stress tests for zero-downtime hot reloading |

### Files to Modify

| File | Change |
|------|--------|
| `cmake/sources.cmake` | Add new MCP and Workflow DSL source files |
| `CMakeLists.txt` | Register new test targets |

---

### Task 1: Create Public Headers & Internal Definitions

**Files:**
- Create: `include/csilk/protocols/mcp.h`, `include/csilk/app/workflow_dsl.h`, `src/protocols/mcp/mcp_internal.h`

- [ ] **Step 1: Create `include/csilk/protocols/mcp.h`**
Define `csilk_mcp_server_t` and `csilk_mcp_client_t` public functions according to spec.

- [ ] **Step 2: Create `include/csilk/app/workflow_dsl.h`**
Define `csilk_wf_from_json`, `csilk_wf_to_json`, and `csilk_wf_manager_t` public functions according to spec.

- [ ] **Step 3: Create `src/protocols/mcp/mcp_internal.h`**
Define `csilk_mcp_msg_t`, JSON-RPC internal error codes, and transport opaque pointers.

---

### Task 2: Implement JSON-RPC 2.0 Parser & Encoder (`mcp_jsonrpc.c`)

**Files:**
- Create: `src/protocols/mcp/mcp_jsonrpc.c`
- Create: `tests/test_mcp_jsonrpc.c`

- [ ] **Step 1: Implement `mcp_jsonrpc.c`**
Parsing, serialization, error frame construction for RFC 4627 JSON-RPC 2.0.

- [ ] **Step 2: Write unit test `test_mcp_jsonrpc.c`**
Verify valid/invalid JSON-RPC 2.0 parsing, notifications, and error frames.

- [ ] **Step 3: Build and run test**
Verify `test_mcp_jsonrpc` passes.

---

### Task 3: Implement MCP Server (`mcp_server.c`)

**Files:**
- Create: `src/protocols/mcp/mcp_server.c`

- [ ] **Step 1: Implement MCP Handler Handshakes**
`initialize`, `tools/list`, `tools/call`, `prompts/list`, `prompts/get`.

- [ ] **Step 2: Implement Stdio Transport**
Asynchronous non-blocking pipe I/O for stdout/stdin.

- [ ] **Step 3: Implement SSE Transport**
Bind GET `/mcp/sse` and POST `/mcp/message` routes to `csilk_app_t`.

---

### Task 4: Implement MCP Client (`mcp_client.c`)

**Files:**
- Create: `src/protocols/mcp/mcp_client.c`
- Create: `tests/test_mcp_server_client.c`

- [ ] **Step 1: Implement MCP Client Connection**
Support Stdio pipe spawn and SSE HTTP client connections.

- [ ] **Step 2: Implement Tool Auto-Import**
Fetch `tools/list` from remote MCP server and dynamically wrap into `csilk_wf_node_t` tool handlers.

- [ ] **Step 3: Write integration test `test_mcp_server_client.c`**
Verify Client <-> Server loopback tool invocation.

---

### Task 5: Implement Declarative Workflow DSL (`workflow_dsl.c`)

**Files:**
- Create: `src/workflow/workflow_dsl.c`
- Create: `tests/test_workflow_dsl.c`

- [ ] **Step 1: Implement `csilk_wf_from_json()` and `csilk_wf_from_file()`**
Parse JSON AST, build DAG, and perform cycle detection via DFS.

- [ ] **Step 2: Implement `csilk_wf_to_json()`**
Export active `csilk_wf_t` instances back into JSON DSL format.

- [ ] **Step 3: Write test `test_workflow_dsl.c`**
Verify parsing, cycle detection error handling, and JSON exporting.

---

### Task 6: Implement Atomic Workflow Manager & Hot Reloading (`workflow_manager.c`)

**Files:**
- Create: `src/workflow/workflow_manager.c`
- Create: `tests/test_workflow_hotreload.c`

- [ ] **Step 1: Implement Workflow Registry & Reference Counting**
`csilk_wf_manager_t` struct with atomic pointer swapping.

- [ ] **Step 2: Write stress test `test_workflow_hotreload.c`**
Verify continuous execution during hot swapping with zero in-flight failures.

---

### Task 7: Implement WebSocket Live Debugger (`workflow_debug.c`)

**Files:**
- Create: `src/workflow/workflow_debug.c`

- [ ] **Step 1: Implement Debugger WebSocket Endpoint**
Mount `/api/v1/workflows/:name/debug` route.

- [ ] **Step 2: Implement Event Dispatcher & Step Control**
Broadcast `node_start`, `node_complete`, `mcp_tool_call` and handle `pause`, `resume`, `step`, `set_breakpoint`.

---

### Task 8: CMake Integration & Final Verification

**Files:**
- Modify: `cmake/sources.cmake`, `CMakeLists.txt`

- [ ] **Step 1: Add new sources to `cmake/sources.cmake`**
- [ ] **Step 2: Full clean rebuild and test execution**
Run `make check-format` and `ctest --output-on-failure`.
