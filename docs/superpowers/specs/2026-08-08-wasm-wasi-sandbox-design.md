# WASM/WASI Sandbox & Plugin Engine Design Specification

## Overview

This specification defines the architecture, components, API contracts, and memory/CPU isolation mechanisms for integrating a lightweight, zero-dependency **WebAssembly (WASM) & WASI Plugin Sandbox Engine** into `csilk` (server-c).

The system enables secure execution of untrusted third-party MCP tools and workflow DAG nodes under strict memory limits (Linear Memory caps) and CPU instruction fuel budgeting, eliminating risks of infinite loops or wild memory pointer access.

---

## 1. System Architecture & Module Boundaries

### 1.1 Directory Structure

```
include/csilk/
  └── core/
      └── wasm.h              # Public WASM sandbox API header

src/
  └── core/
      └── plugin/
          ├── wasm_plugin.c   # Plugin lifecycle & Workflow/MCP binding
          ├── wasm_vm.c       # WASM bytecode parser, stack frame & opcode interpreter
          ├── wasm_wasi.c     # WASI preview1 host function bindings
          └── wasm_internal.h # Internal memory pools, operand stacks & fuel counters
```

### 1.2 Safety & Isolation Guarantees

1. **Zero External Dependencies**: Implemented natively in C23 without dynamic linking to heavy external JIT runtimes (e.g., `libwasmtime.so`).
2. **Memory Boundary Guard**: Linear memory accesses are bounds-checked against `offset + size <= max_memory_bytes`. Violations trigger `CSILK_WASM_TRAP_OUT_OF_BOUNDS`.
3. **Instruction Fuel Budgeting**: Every loop iteration, branch, and call decrements a per-execution `fuel` counter. Exhaustion immediately halts execution with `CSILK_WASM_TRAP_FUEL_EXHAUSTED`.

---

## 2. WASM Module Parsing & Memory Layout

### 2.1 Binary Header Validation (`wasm_vm.c`)

* **Magic Number**: `0x6d736100` (`\0asm`)
* **Version**: `0x01000000`

Supported WASM sections:
* `Type Section (1)`: Function signatures.
* `Import Section (2)`: WASI preview1 and host functions.
* `Function Section (3)`: Internal function indices.
* `Memory Section (5)`: Min and max 64KB pages.
* `Export Section (7)`: Exported functions (e.g., `malloc`, `free`, `run`).
* `Code Section (10)`: Opcode streams.

### 2.2 Linear Memory Management

Memory is managed in 64KB pages:

```c
typedef struct {
    uint8_t*  data;          /* Linear memory block */
    uint32_t  initial_pages; /* Initial page count (e.g. 1 = 64KB) */
    uint32_t  max_pages;     /* Hard cap page count (e.g. 1024 = 64MB) */
    size_t    current_size;  /* Current allocated bytes */
} csilk_wasm_memory_t;
```

---

## 3. Interpreter & Fuel Budgeting

### 3.1 Stack Frame & Value Union

```c
typedef union {
    int32_t  i32;
    int64_t  i64;
    float    f32;
    double   f64;
} csilk_wasm_val_t;

typedef struct {
    csilk_wasm_val_t stack[256]; /* Operand stack */
    size_t           sp;         /* Stack pointer */
    uint64_t         fuel;       /* Remaining CPU fuel counter */
} csilk_wasm_exec_ctx_t;
```

---

## 4. WASI & Host Bindings

### 4.1 WASI Subsets (`wasm_wasi.c`)

* `wasi_snapshot_preview1.fd_write`: Diverts `stdout`/`stderr` prints safely to `csilk` logger.
* `wasi_snapshot_preview1.clock_time_get`: Millisecond monotonic timer.
* `wasi_snapshot_preview1.proc_exit`: Intercepts WASM module exit codes.

### 4.2 Host Functions

```c
uint32_t csilk_host_get_input(uint32_t memory_offset, uint32_t max_len);
int32_t  csilk_host_set_output(uint32_t memory_offset, uint32_t len);
void     csilk_host_log(uint32_t level, uint32_t memory_offset, uint32_t len);
```

---

## 5. Public API Contracts

### 5.1 `include/csilk/core/wasm.h`

```c
#ifndef CSILK_WASM_H
#define CSILK_WASM_H

#include <stddef.h>
#include <stdint.h>
#include "csilk/app/workflow_dsl.h"
#include "csilk/protocols/mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_wasm_plugin_s csilk_wasm_plugin_t;

typedef enum {
    CSILK_WASM_OK                  = 0,
    CSILK_WASM_ERR_INVALID_MAGIC   = -1,
    CSILK_WASM_ERR_INVALID_VERSION = -2,
    CSILK_WASM_TRAP_OUT_OF_BOUNDS  = -3,
    CSILK_WASM_TRAP_FUEL_EXHAUSTED = -4,
    CSILK_WASM_TRAP_EXECUTION_ERR  = -5
} csilk_wasm_status_t;

typedef struct {
    uint32_t max_memory_pages; /* Max pages (default 1024 = 64MB) */
    uint64_t fuel_limit;       /* Max fuel (default 1,000,000) */
} csilk_wasm_config_t;

/**
 * @brief Loads a WASM plugin from file path.
 */
csilk_wasm_plugin_t* csilk_wasm_plugin_load_file(const char* filepath, const csilk_wasm_config_t* config);

/**
 * @brief Frees a WASM plugin instance.
 */
void csilk_wasm_plugin_free(csilk_wasm_plugin_t* plugin);

/**
 * @brief Executes an exported function in the WASM module.
 */
char* csilk_wasm_plugin_exec(csilk_wasm_plugin_t* plugin,
                             const char*          func_name,
                             const char*          json_input,
                             char*                err_buf,
                             size_t               err_len);

/**
 * @brief Binds a WASM plugin step to a Workflow DAG.
 */
int csilk_wf_add_wasm_node(csilk_wf_t* wf, const char* node_id, const char* wasm_filepath);

/**
 * @brief Registers a WASM plugin as an MCP Tool.
 */
int csilk_mcp_server_register_wasm_tool(csilk_mcp_server_t* server,
                                        const char*          wasm_filepath,
                                        const char*          tool_name,
                                        const char*          description);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_WASM_H */
```

---

## 6. Test Plan

1. **`test_wasm_vm.c`**: Test WASM bytecode header parsing, opcode execution, and operand stack evaluation.
2. **`test_wasm_fuel.c`**: Execute an infinite loop WASM module (`loop ... br 0`), asserting graceful trap `CSILK_WASM_TRAP_FUEL_EXHAUSTED`.
3. **`test_wasm_wasi.c`**: Test WASI `fd_write` output capturing and host function data exchange.
4. **`test_wf_wasm_node.c`**: Integration test executing a WASM plugin node inside a Workflow DAG pipeline.
