# JSON Value Object & Safe View Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the dangerous 2MB `tls_view_ring[65536]` buffer, implement 16-byte Small Value Object APIs (`csilk_json_t` value struct / `csilk_json_*_v`), maintain 100% backward compatibility for existing code, and eliminate ring overwrite data corruption.

**Architecture:**
- `include/csilk/core/json.h`: Publicly define `csilk_json_t` as a compact 16-byte struct with `u` (val), `doc` (doc), and `flags`.
- Value Object APIs (`csilk_json_get_v`, `csilk_json_array_get_v`, `csilk_json_is_valid`, etc.) return by value in CPU registers (`rax:rdx`).
- Pointer APIs (`csilk_json_parse`, `csilk_json_object`, etc.) use clean heap management without `tls_view_ring`.
- `tests/core/test_json_accessor_bench.c`: Comprehensive benchmark measuring cycles/get, nested access, array iterations, and multi-threaded TSAN verification.

---

### Task 1: Update JSON Type Definitions & Value Object Header Declarations

**Files:**
- Modify: `include/csilk/core/json.h`
- Modify: `src/core/json/json_internal.h`

- [ ] **Step 1: Define `csilk_json_t` as a transparent 16-byte struct in `include/csilk/core/json.h`**
```c
typedef struct csilk_json_s {
    union {
        void* raw;
        void* ival;
        void* mval;
    } u;
    union {
        void* raw;
        void* idoc;
        void* mdoc;
    } doc;
    uint32_t flags;
} csilk_json_t;
```

- [ ] **Step 2: Add Value Object Function Declarations**
```c
csilk_json_t csilk_json_get_v(csilk_json_t obj, const char* key);
csilk_json_t csilk_json_array_get_v(csilk_json_t arr, size_t index);
bool         csilk_json_is_valid(csilk_json_t v);
```

- [ ] **Step 3: Update `src/core/json/json_internal.h`**
Update internal flags and helper signatures.

- [ ] **Step 4: Commit Task 1**
```bash
git add include/csilk/core/json.h src/core/json/json_internal.h
git commit -m "refactor(json): ♻️ define 16-byte value struct and value accessor declarations"
```

---

### Task 2: Implement Value Accessors & Refactor Internal Helpers

**Files:**
- Modify: `src/core/json/json_internal.c`
- Modify: `src/core/json/json_access.c`
- Modify: `src/core/json/json_factory.c`
- Modify: `src/core/json/json_parse.c`
- Modify: `src/core/json/json_free.c`
- Modify: `src/core/json/json_array.c`
- Modify: `src/core/json/json_object.c`

- [ ] **Step 1: Eliminate `tls_view_ring` in `src/core/json/json_internal.c`**
- [ ] **Step 2: Implement `csilk_json_get_v`, `csilk_json_array_get_v`, `csilk_json_is_valid` in `src/core/json/json_access.c`**
- [ ] **Step 3: Update parsing and factory methods with clean root handle allocation**
- [ ] **Step 4: Commit Task 2**
```bash
git add src/core/json/
git commit -m "feat(json): ✨ eliminate TLS ring and implement zero-overhead value accessors"
```

---

### Task 3: Add JSON Accessor Benchmark & Stress Test

**Files:**
- Create: `tests/core/test_json_accessor_bench.c`
- Modify: `cmake/tests.cmake`

- [ ] **Step 1: Implement `tests/core/test_json_accessor_bench.c`**
Benchmark:
1. Flat object key lookup (Value object vs Pointer).
2. Deep nested object lookup.
3. 100,000 element array iteration (verifying 0 ring overwrite corruption).
4. Multi-threaded async view retention and cross-thread read safety under TSAN.
- [ ] **Step 2: Register test in `cmake/tests.cmake`**
- [ ] **Step 3: Build and run test**
```bash
cmake -B build -S . && cmake --build build --target test_json_accessor_bench
./build/test_json_accessor_bench
```
- [ ] **Step 4: Commit Task 3**
```bash
git add tests/core/test_json_accessor_bench.c cmake/tests.cmake
git commit -m "test(json): ✅ add JSON accessor benchmark and ring overwrite verification"
```

---

### Task 4: Full Test Suite, ASAN/TSAN Verification & Formatting

- [ ] **Step 1: Run full unit test suite**
- [ ] **Step 2: Run ASAN test suite**
- [ ] **Step 3: Run TSAN test suite**
- [ ] **Step 4: Code formatting**
- [ ] **Step 5: Final commit**
