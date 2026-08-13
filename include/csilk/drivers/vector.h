#pragma once
/**
 * @file vector.h
 * @brief Unified pluggable interface for Vector Database integration.
 *
 * Provides an abstraction for vector indexing and similarity search (RAG).
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include "csilk/core/json.h"

/** @brief A single vector point. */
typedef struct {
    const char*   id;        /**< String ID (UUID usually). */
    const float*  vector;    /**< Floating point vector array. */
    size_t        dimension; /**< Vector dimension. */
    csilk_json_t* payload;   /**< Optional metadata payload (JSON). */
} csilk_vector_point_t;

/** @brief A single vector search result. */
typedef struct {
    char*         id;      /**< Found point ID (heap-allocated). */
    float         score;   /**< Similarity score. */
    csilk_json_t* payload; /**< Metadata payload (heap-allocated). */
} csilk_vector_search_result_t;

/** @brief Response data for a vector search. */
typedef struct {
    csilk_vector_search_result_t* results;       /**< Array of results (heap-allocated). */
    size_t                        count;         /**< Number of results. */
    char*                         error_message; /**< Error message (if failed). */
} csilk_vector_search_response_t;

/** @brief Opaque handle for a Vector DB instance. */
typedef struct csilk_vector_db_s csilk_vector_db_t;

/**
 * @brief Virtual function table implemented by each Vector DB driver.
 */
typedef struct {
    const char* name; /**< Driver identifier (e.g., "qdrant"). */

    /** @brief Initialize driver-specific state.
     *  @param endpoint  Endpoint URL (e.g., "http://localhost:6333").
     *  @param api_key   Optional API key.
     *  @return Opaque driver state handle, or NULL on failure. */
    void* (*init)(const char* endpoint, const char* api_key);

    /** @brief Upsert (insert or update) points into a collection.
     *  @param state      Driver state.
     *  @param collection Collection name.
     *  @param points     Array of points to upsert.
     *  @param count      Number of points.
     *  @return 0 on success, -1 on failure. */
    int (*upsert)(void*                       state,
                  const char*                 collection,
                  const csilk_vector_point_t* points,
                  size_t                      count);

    /** @brief Search for similar vectors.
     *  @param state      Driver state.
     *  @param collection Collection name.
     *  @param vector     Query vector.
     *  @param dimension  Dimension of the query vector.
     *  @param limit      Maximum number of results to return.
     *  @param res        [out] Search response.
     *  @return 0 on success, -1 on failure. */
    int (*search)(void*                           state,
                  const char*                     collection,
                  const float*                    vector,
                  size_t                          dimension,
                  int                             limit,
                  csilk_vector_search_response_t* res);

    /** @brief Clean up all driver-specific state. */
    void (*free)(void* state);
} csilk_vector_db_driver_t;

/** @brief Probes CPU support for AVX2 SIMD instructions. */
int csilk_simd_has_avx2(void);

/** @brief Create a native embedded SIMD HNSW vector database driver.
 *  @param dim     Vector dimension (e.g. 1536).
 *  @param metric  Distance metric: 0=COSINE, 1=L2, 2=IP.
 *  @return Handle or NULL. */
csilk_vector_db_t* csilk_vector_db_new_embedded(size_t dim, int metric);

/** @brief Create a new Vector DB instance.
 * @param driver_name "qdrant", "milvus", "embedded", etc.
 * @param endpoint    API endpoint.
 * @param api_key     Optional API key.
 * @return Handle or NULL. */
csilk_vector_db_t*
csilk_vector_db_new(const char* driver_name, const char* endpoint, const char* api_key);

/** @brief Upsert points. */
int csilk_vector_db_upsert(csilk_vector_db_t*          db,
                           const char*                 collection,
                           const csilk_vector_point_t* points,
                           size_t                      count);

/** @brief Search points. */
int csilk_vector_db_search(csilk_vector_db_t*              db,
                           const char*                     collection,
                           const float*                    vector,
                           size_t                          dimension,
                           int                             limit,
                           csilk_vector_search_response_t* res);

/** @brief Free Vector DB instance. */
void csilk_vector_db_free(csilk_vector_db_t* db);

/** @brief Free a search response structure. */
void csilk_vector_search_response_free(csilk_vector_search_response_t* res);

/** @brief Register a driver. */
void csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver);
