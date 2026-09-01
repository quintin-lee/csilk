# JSON and AI ABI Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the JSON API and AI drivers use one stable pointer-based ownership contract and eliminate the Python AI crash.

**Architecture:** Keep `csilk_json_t` opaque to public consumers. JSON factory functions return heap-owned pointers; child lookup returns borrowed pointers; object/array insertion transfers ownership only after successful insertion. Remove public value-object APIs from the AI path and migrate internal callers incrementally. AI ctypes structures must mirror `include/csilk/drivers/ai.h` exactly, including field order and native alignment.

**Tech Stack:** C23, yyjson, ctypes, CMake, CTest, pytest.

---

### Task 1: Add ABI and ownership regression coverage

**Files:**
- Create: `tests/core/test_json_ai_abi.c`
- Create: `python/tests/test_ai_abi.py`
- Modify: `cmake/tests.cmake`

- [ ] Add a C test that constructs an object, adds strings and arrays, serializes it, and frees the root exactly once.
- [ ] Add a Python test that asserts `ctypes.sizeof` and field offsets for `CsilkAiMessage` and `CsilkAiChatRequest` against documented C offsets.
- [ ] Register the C test with CTest.
- [ ] Run the C test before implementation and require it to fail if ownership or ABI is invalid.

### Task 2: Restore a genuinely opaque public JSON type

**Files:**
- Modify: `include/csilk/core/json/json.h`
- Modify: `src/core/json/json_internal.h`
- Modify: `src/core/json/json_internal.c`

- [ ] Replace the public struct definition with `typedef struct csilk_json_s csilk_json_t;`.
- [ ] Keep the concrete struct definition only in `json_internal.h`.
- [ ] Remove public value-object declarations (`*_v`) unless required by a separately documented ABI; pointer APIs remain authoritative.
- [ ] Keep factory handles heap-owned and make `csilk_json_free()` idempotent by nulling ownership before deallocation.

### Task 3: Normalize JSON ownership transfers

**Files:**
- Modify: `src/core/json/json_object.c`
- Modify: `src/core/json/json_array.c`
- Modify: `src/core/json/json_free.c`
- Modify: `src/core/json/json_serialize.c`

- [ ] Validate object and value pointers before every yyjson call.
- [ ] On insertion failure, retain ownership with the caller and do not mutate the child handle.
- [ ] On successful insertion, transfer the child document/value into the parent exactly once and invalidate the child wrapper.
- [ ] Ensure borrowed child views never carry owner flags and are never individually freed.
- [ ] Run `test_json`, `test_json_ext`, and the new ABI regression test.

### Task 4: Migrate AI drivers to the normalized JSON contract

**Files:**
- Modify: `src/drivers/ai/openai.c`
- Modify: `src/drivers/ai/ollama.c`
- Modify: `src/drivers/ai/ai.c`

- [ ] Build request JSON only through pointer APIs.
- [ ] Check every factory and insertion result.
- [ ] Free temporary child handles only when ownership was not transferred.
- [ ] Free serialized buffers with `free()`/`csilk_free()` according to the existing allocator contract.
- [ ] Add a local mock HTTP endpoint test to verify failed transport returns `RuntimeError` instead of crashing.

### Task 5: Align Python ctypes definitions and lifetimes

**Files:**
- Modify: `python/csilk/lib.py`
- Modify: `python/csilk/ai.py`
- Test: `python/tests/test_ai_safety.py`

- [ ] Match C field order exactly: `role`, `content`, `tool_calls`, `tool_call_count`, `tool_call_id`.
- [ ] Retain Python byte-string references for every pointer field for the full native call.
- [ ] Keep streaming callback references until native completion.
- [ ] Always free response buffers in success and failure paths.
- [ ] Verify the AI safety regression under pytest.

### Task 6: Full validation and documentation

**Files:**
- Modify: `docs/superpowers/specs/2026-08-31-cmake-target-split-phase1-design.md`
- Modify: `docs/superpowers/plans/2026-08-31-cmake-target-split-phase1.md`

- [ ] Build shared and static targets.
- [ ] Run JSON/AI CTest targets and complete non-integration CTest.
- [ ] Run Python tests with pytest and `-W error::ResourceWarning`.
- [ ] Run ASAN/TSAN variants where available.
- [ ] Run format, Mermaid, version-sync, and `git diff --check`.
- [ ] Record known limitations only after reproducing them with the final ABI.
