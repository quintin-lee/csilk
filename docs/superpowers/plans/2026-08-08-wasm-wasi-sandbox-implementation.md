# WASM/WASI Sandbox & Plugin Engine Implementation Plan

Implementation plan for building the native zero-dependency WASM/WASI Plugin Sandbox engine in `csilk`.

## User Review Required

> [!IMPORTANT]
> All code changes must follow C23 standard, keep file sizes under 700 lines, achieve 0 clang-tidy warnings (`make tidy`), and pass 100% of unit/integration tests.

## Proposed Changes

### Core Subsystem: WASM Sandbox Engine (`src/core/plugin/`)

- Public API header: `include/csilk/core/wasm.h`
- Internal types & stacks: `src/core/plugin/wasm_internal.h`
- Bytecode parser & interpreter: `src/core/plugin/wasm_vm.c`
- WASI preview1 host callbacks: `src/core/plugin/wasm_wasi.c`
- Plugin manager & DAG/MCP bindings: `src/core/plugin/wasm_plugin.c`

---

## Execution Plan

### Task 1: Public and Internal Header Files
- Create `include/csilk/core/wasm.h` defining `csilk_wasm_plugin_t`, `csilk_wasm_config_t`, and public function declarations.
- Create `src/core/plugin/wasm_internal.h` defining opcode types, operand stack, linear memory structure, and fuel counters.

### Task 2: WASM Bytecode Parser & Section Scanner
- Implement binary header validation (`\0asm`) and Section scanner (Types, Imports, Functions, Memory, Exports, Code) in `src/core/plugin/wasm_vm.c`.
- Create `tests/core/test_wasm_vm.c` to test section parsing and function index resolution.

### Task 3: Operand Stack Interpreter & Fuel Budgeting
- Implement opcode interpreter loop in `src/core/plugin/wasm_vm.c` with fuel decrement on loop/branch instructions.
- Create `tests/core/test_wasm_fuel.c` executing an infinite loop bytecode module to verify `CSILK_WASM_TRAP_FUEL_EXHAUSTED` truncation.

### Task 4: WASI Preview1 & Host Callback Integration
- Implement `wasi_snapshot_preview1` handlers (`fd_write`, `clock_time_get`, `proc_exit`) and host JSON exchange in `src/core/plugin/wasm_wasi.c`.
- Create `tests/core/test_wasm_wasi.c` testing input/output buffer passing and logger interception.

### Task 5: Workflow DAG & MCP Tool WASM Integration
- Implement `csilk_wf_add_wasm_node` and `csilk_mcp_server_register_wasm_tool` in `src/core/plugin/wasm_plugin.c`.
- Create `tests/workflow/test_wf_wasm_node.c` running a WASM node step inside a Workflow pipeline.

### Task 6: CMake Registration & Quality Assurance
- Update `cmake/sources.cmake` and `cmake/tests.cmake`.
- Run `make format`, `make check-format`, `make tidy`, and `ctest --output-on-failure`.

---

## Verification Plan

```bash
cd build
make format
make check-format
make tidy
make -j4
ctest --output-on-failure
```
