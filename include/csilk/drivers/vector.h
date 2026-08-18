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

/**
 * @brief Insert or update high-dimensional vector points in a collection.
 *
 * @param[in,out] db         Vector DB instance handle.
 * @param[in]     collection Target collection name.
 * @param[in]     points     Array of vector points to upsert.
 * @param[in]     count      Number of points in the @p points array.
 * @return 0 on success, or -1 on error.
 *
 * @note The points array and underlying floats are borrowed for the duration of the call.
 */
int csilk_vector_db_upsert(csilk_vector_db_t*          db,
                           const char*                 collection,
                           const csilk_vector_point_t* points,
                           size_t                      count);

/**
 * @brief Search for nearest vector points using cosine, L2 or inner product similarity.
 *
 * @param[in,out] db         Vector DB instance handle.
 * @param[in]     collection Collection name to search within.
 * @param[in]     vector     Query vector float array.
 * @param[in]     dimension  Dimensionality of the query vector (must match index dimension).
 * @param[in]     limit      Maximum number of top-k nearest neighbors to return.
 * @param[out]    res        Pointer to response structure to be populated.
 * @return 0 on success, or -1 on error (with error_message set in @p res).
 *
 * @note Memory Ownership: The @p res structure contains heap-allocated results and strings.
 *       The caller MUST release it by calling csilk_vector_search_response_free(res).
 */
int csilk_vector_db_search(csilk_vector_db_t*              db,
                           const char*                     collection,
                           const float*                    vector,
                           size_t                          dimension,
                           int                             limit,
                           csilk_vector_search_response_t* res);

/**
 * @brief Destroy a Vector DB instance and free its underlying resources.
 *
 * @param[in,out] db Vector DB handle to free (safe to call with NULL).
 */
void csilk_vector_db_free(csilk_vector_db_t* db);

/**
 * @brief Free heap memory allocated inside a vector search response structure.
 *
 * Frees result IDs, metadata JSON payloads, error message strings, and the results array.
 *
 * @param[in,out] res Pointer to the response structure to clean up.
 */
void csilk_vector_search_response_free(csilk_vector_search_response_t* res);

/**
 * @brief Register a pluggable Vector DB driver implementation into the global registry.
 *
 * @param[in] driver Driver vtable structure (must remain valid for the process lifetime).
 */
void csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver);
