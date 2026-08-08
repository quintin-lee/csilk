# Embedded SIMD Vector Index Engine Implementation Plan

Implementation plan for building the native zero-dependency SIMD-accelerated HNSW vector index engine in `csilk`.

## User Review Required

> [!IMPORTANT]
> All code changes must follow C23 standard, keep file sizes under 700 lines, achieve 0 clang-tidy warnings (`make tidy`), and pass 100% of unit/integration tests.

## Proposed Changes

### Core Subsystem: Embedded Vector Index (`src/drivers/vector/`)

- Public API header: `include/csilk/drivers/vector.h`
- Internal graph & memory structs: `src/drivers/vector/vector_internal.h`
- AVX2 / FMA SIMD kernels & 32-byte allocator: `src/drivers/vector/vector_simd.c`
- HNSW skip-graph index & ANN priority search: `src/drivers/vector/vector_hnsw.c`
- Factory & unified driver wrapper: `src/drivers/vector/vector.c`

---

## Execution Plan

### Task 1: Public and Internal Header Files
- Update `include/csilk/drivers/vector.h` with `CSILK_VECTOR_DRIVER_EMBEDDED` and function prototypes.
- Create `src/drivers/vector/vector_internal.h` defining 32-byte aligned SIMD pointers, HNSW node structures, and distance function pointer types.

### Task 2: AVX2 / FMA SIMD Distance Kernels & Aligned Memory
- Implement `csilk_aligned_alloc`, `csilk_simd_has_avx2`, `csilk_simd_cosine_distance_avx2`, `csilk_simd_l2_distance_avx2`, and scalar fallbacks in `src/drivers/vector/vector_simd.c`.
- Create `tests/drivers/vector/test_vector_simd.c` testing 32-byte alignment and validating SIMD distance accuracy against scalar loop.

### Task 3: HNSW Multi-Layer Skip-Graph Index & ANN Beam Search
- Implement `csilk_hnsw_node_create`, layer probability assignment, top-down greedy search, heuristic edge insertion, and priority queue search in `src/drivers/vector/vector_hnsw.c`.
- Create `tests/drivers/vector/test_vector_hnsw.c` inserting 1,000 128-dimensional vectors and verifying ANN recall ($\ge 98\%$).

### Task 4: Vector Driver Factory & Unified Interface Integration
- Implement `csilk_vector_db_new_embedded`, `csilk_vector_db_upsert`, `csilk_vector_db_search`, and cleanup methods in `src/drivers/vector/vector.c`.
- Create `tests/drivers/vector/test_vector_db_embedded.c` testing end-to-end `upsert` and `search` using `csilk_vector_db_t`.

### Task 5: CMake Registration & Quality Assurance
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
