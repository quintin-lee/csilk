# Embedded SIMD Vector Index Engine Design Specification

## Overview

This specification defines the architecture, data structures, SIMD hardware acceleration kernels, and HNSW graph indexing algorithms for integrating an **Embedded SIMD Vector Index Engine (`CSILK_VECTOR_DRIVER_EMBEDDED`)** into `csilk` (server-c).

The system enables zero-dependency, sub-millisecond ($< 0.5 \text{ ms}$) approximate nearest neighbor (ANN) vector search for AI Agent memory recall and RAG retrieval directly within the C23 process, eliminating the need for external vector databases (Qdrant/Milvus).

---

## 1. System Architecture & Module Boundaries

### 1.1 Directory Structure

```
include/csilk/
  └── drivers/
      └── vector.h             # Extended vector driver public header

src/
  └── drivers/
      └── vector/
          ├── vector_simd.c    # AVX2/FMA SIMD distance kernels & 32-byte alignment
          ├── vector_hnsw.c    # HNSW (Hierarchical Navigable Small World) graph engine
          ├── vector.c         # Vector driver factory & wrapper bindings
          └── vector_internal.h# Internal structs, graph adjacency lists & SIMD pointers
```

### 1.2 Performance & Safety Guarantees

1. **Zero External Dependencies**: Pure C23 implementation without dynamic linking to external C++ libraries.
2. **32-Byte Aligned Memory**: Vector float arrays are allocated on 32-byte boundaries (`aligned_alloc(32, ...)`) to enable penalty-free 256-bit AVX2 vector loads.
3. **Runtime CPU Probing**: Automatically selects AVX2/FMA kernels (`_mm256_fmadd_ps`) when hardware support is detected (`__builtin_cpu_supports("avx2")`), with seamless fallback to an 8x unrolled C loop.

---

## 2. AVX2/FMA SIMD Distance Kernels (`vector_simd.c`)

### 2.1 Supported Metrics

* **Cosine Distance**: $d_{cos}(A, B) = 1.0 - \frac{\sum A_i B_i}{\sqrt{\sum A_i^2} \sqrt{\sum B_i^2}}$
* **Euclidean Distance (L2 Squared)**: $d_{L2}(A, B) = \sum (A_i - B_i)^2$
* **Inner Product**: $d_{IP}(A, B) = -\sum A_i B_i$

### 2.2 Memory Alignment Allocator

```c
void* csilk_aligned_alloc(size_t alignment, size_t size);
void  csilk_aligned_free(void* ptr);
```

---

## 3. HNSW Multi-Layer Skip-Graph Index (`vector_hnsw.c`)

### 3.1 Node & Index Structures

```c
typedef struct csilk_hnsw_node_s {
    uint32_t  id;             /* Internal numeric ID */
    char*     doc_id;         /* External Document / Payload ID */
    float*    vector;         /* 32-byte aligned float array */
    int       level;          /* Maximum assigned layer level */
    uint32_t** neighbors;     /* Adjacency list per level */
    uint16_t* neighbor_count; /* Neighbor count per level */
} csilk_hnsw_node_t;

typedef struct {
    csilk_hnsw_node_t** nodes;
    size_t              node_count;
    size_t              node_capacity;
    uint32_t            enter_node_id; /* Top layer entry point ID */
    int                 max_level;     /* Maximum graph level */
    size_t              dim;           /* Vector dimension */
    size_t              M;             /* Max neighbors per node per level (default 16) */
    size_t              ef_construction;/* Search depth during construction (default 200) */
    size_t              ef_search;     /* Search depth during query (default 64) */
    csilk_simd_dist_fn  dist_fn;       /* Distance evaluation function pointer */
    csilk_mutex_t       mutex;
} csilk_hnsw_index_t;
```

---

## 4. Public API Contracts (`include/csilk/drivers/vector.h`)

```c
#ifndef CSILK_DRIVERS_VECTOR_H
#define CSILK_DRIVERS_VECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CSILK_VECTOR_METRIC_COSINE = 0,
    CSILK_VECTOR_METRIC_L2 = 1,
    CSILK_VECTOR_METRIC_IP = 2
} csilk_vector_metric_type_t;

typedef enum {
    CSILK_VECTOR_DRIVER_QDRANT,
    CSILK_VECTOR_DRIVER_MILVUS,
    CSILK_VECTOR_DRIVER_EMBEDDED
} csilk_vector_driver_type_t;

typedef struct csilk_vector_db_s csilk_vector_db_t;

typedef struct {
    char**   doc_ids;
    float*   scores;
    size_t   count;
} csilk_vector_search_response_t;

/**
 * @brief Probes CPU support for AVX2 SIMD instructions.
 */
int csilk_simd_has_avx2(void);

/**
 * @brief Creates a native embedded SIMD HNSW vector database driver instance.
 */
csilk_vector_db_t* csilk_vector_db_new_embedded(size_t dim, csilk_vector_metric_type_t metric);

/**
 * @brief Inserts or updates a vector in the embedded index.
 */
int csilk_vector_db_upsert(csilk_vector_db_t* db, const char* doc_id, const float* vector, size_t dim);

/**
 * @brief Performs k-NN vector similarity search.
 */
int csilk_vector_db_search(csilk_vector_db_t*               db,
                           const float*                    query_vector,
                           size_t                          dim,
                           size_t                          top_k,
                           csilk_vector_search_response_t* res);

/**
 * @brief Frees search response memory.
 */
void csilk_vector_search_response_free(csilk_vector_search_response_t* res);

/**
 * @brief Frees vector database driver instance.
 */
void csilk_vector_db_free(csilk_vector_db_t* db);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_DRIVERS_VECTOR_H */
```

---

## 5. Test Plan

1. **`test_vector_simd.c`**: Verify 32-byte alignment allocator and test AVX2 Cosine, L2, and IP SIMD calculations against scalar reference implementations (error $< 10^{-5}$).
2. **`test_vector_hnsw.c`**: Insert 1,000 128-dimensional vectors, test HNSW graph building and ANN search recall ($\ge 98\%$).
3. **`test_vector_db_embedded.c`**: Integration test using `csilk_vector_db_t` driver for full `upsert`, `search`, and response cleanup workflow.
