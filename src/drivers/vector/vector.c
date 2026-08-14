/**
 * @file vector.c
 * @brief Core registry and implementations for Vector DB drivers.
 * @copyright MIT License
 */

#include "csilk/drivers/vector.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_VECTOR_DRIVERS = 4 };

static const csilk_vector_db_driver_t* vector_drivers[MAX_VECTOR_DRIVERS];
static int                             vector_driver_count = 0;

struct csilk_vector_db_s {
    const csilk_vector_db_driver_t* driver;
    void*                           state;
};

/**
 * @brief Register a vector-DB driver in the global registry.
 *
 * The driver vtable is stored by pointer (not copied) and must remain valid
 * for the process lifetime. At most MAX_VECTOR_DRIVERS (4) drivers can be
 * registered.
 *
 * @param driver Driver vtable to register (name must be set). */
void
csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver)
{
    if (vector_driver_count < MAX_VECTOR_DRIVERS) {
        vector_drivers[vector_driver_count++] = driver;
    }
}

extern void csilk_vector_qdrant_init(void);
extern void csilk_vector_milvus_init(void);

#include "vector_internal.h"

/**
 * @brief Create an in-memory embedded vector DB backed by an HNSW index.
 *
 * Allocates a csilk_vector_db_t whose state is an HNSW index built with the
 * given dimensionality and distance metric. The returned handle has no driver
 * vtable, so upsert/search/free dispatch to the embedded HNSW implementation.
 *
 * @param dim    Vector dimensionality (must be non-zero).
 * @param metric Distance metric selector passed to the HNSW index.
 * @return Newly allocated DB handle, or NULL on OOM or invalid dimension.
 * @note The caller owns the handle and must free it with
 *       csilk_vector_db_free(). */
csilk_vector_db_t*
csilk_vector_db_new_embedded(size_t dim, int metric)
{
    csilk_hnsw_index_t* index = csilk_hnsw_index_new(dim, metric);
    if (!index) {
        return NULL;
    }

    csilk_vector_db_t* db = calloc(1, sizeof(csilk_vector_db_t));
    if (!db) {
        csilk_hnsw_index_free(index);
        return NULL;
    }

    db->state = index;
    return db;
}

/**
 * @brief Create a vector DB, either embedded or via a named remote driver.
 *
 * For the literal driver name "embedded" this delegates to
 * csilk_vector_db_new_embedded() with a default 1536-dimension / cosine
 * configuration. Otherwise the driver registry is lazily initialised (Qdrant
 * and Milvus) and the matching driver's init() callback is invoked with the
 * endpoint and API key.
 *
 * @param driver_name Backend name ("embedded", "qdrant", "milvus", ...).
 * @param endpoint    Remote endpoint URL (ignored for embedded).
 * @param api_key     Optional API key (ignored for embedded).
 * @return Newly allocated DB handle, or NULL if the driver is unknown,
 *         init() fails, or allocation fails. */
csilk_vector_db_t*
csilk_vector_db_new(const char* driver_name, const char* endpoint, const char* api_key)
{
    if (!driver_name) {
        return NULL;
    }

    if (strcmp(driver_name, "embedded") == 0) {
        return csilk_vector_db_new_embedded(1536, 0);
    }

    static int initialized = 0;
    if (!initialized) {
        csilk_vector_qdrant_init();
        csilk_vector_milvus_init();
        initialized = 1;
    }

    const csilk_vector_db_driver_t* driver = NULL;
    for (int i = 0; i < vector_driver_count; i++) {
        if (strcmp(vector_drivers[i]->name, driver_name) == 0) {
            driver = vector_drivers[i];
            break;
        }
    }

    if (!driver) {
        return NULL;
    }

    void* state = driver->init(endpoint, api_key);
    if (!state) {
        return NULL;
    }

    csilk_vector_db_t* db = malloc(sizeof(csilk_vector_db_t));
    if (!db) {
        driver->free(state);
        return NULL;
    }

    db->driver = driver;
    db->state = state;
    return db;
}

/**
 * @brief Insert or update a batch of vectors in the database.
 *
 * If the handle has a driver vtable with an upsert() callback, that is used;
 * otherwise the points are inserted into the embedded HNSW index keyed by
 * their id. The operation is all-or-nothing for the embedded path (the first
 * failing insert aborts and returns -1).
 *
 * @param db         Database handle (must not be NULL).
 * @param collection Collection/namespace name (passed to remote drivers).
 * @param points     Array of vectors to upsert (must not be NULL).
 * @param count      Number of points (must be > 0).
 * @return 0 on success, -1 on invalid arguments or a failed insert. */
int
csilk_vector_db_upsert(csilk_vector_db_t*          db,
                       const char*                 collection,
                       const csilk_vector_point_t* points,
                       size_t                      count)
{
    if (!db || !points || count == 0) {
        return -1;
    }
    if (db->driver && db->driver->upsert) {
        return db->driver->upsert(db->state, collection, points, count);
    } else if (db->state) {
        csilk_hnsw_index_t* idx = (csilk_hnsw_index_t*)db->state;
        for (size_t i = 0; i < count; i++) {
            if (csilk_hnsw_insert(idx, points[i].id, points[i].vector) != 0) {
                return -1;
            }
        }
        return 0;
    }
    return -1;
}

/**
 * @brief Search the database for the @p limit nearest vectors to @p vector.
 *
 * Dispatches to the driver's search() callback when present, otherwise to the
 * embedded HNSW index. On success the response's results array is
 * heap-allocated and populated with matched ids and scores; the caller must
 * release it with csilk_vector_search_response_free().
 *
 * @param db          Database handle (must not be NULL).
 * @param collection  Collection/namespace name (passed to remote drivers).
 * @param vector      Query vector (must not be NULL).
 * @param dimension   Dimensionality of @p vector.
 * @param limit       Maximum number of results (must be > 0).
 * @param[out] res    Response struct to populate (must not be NULL).
 * @return 0 on success, -1 on invalid arguments or search failure. */
int
csilk_vector_db_search(csilk_vector_db_t*              db,
                       const char*                     collection,
                       const float*                    vector,
                       size_t                          dimension,
                       int                             limit,
                       csilk_vector_search_response_t* res)
{
    if (!db || !vector || !res || limit <= 0) {
        return -1;
    }
    if (db->driver && db->driver->search) {
        return db->driver->search(db->state, collection, vector, dimension, limit, res);
    } else if (db->state) {
        csilk_hnsw_index_t* idx = (csilk_hnsw_index_t*)db->state;
        char**              doc_ids = NULL;
        float*              scores = NULL;
        size_t              count = 0;
        if (csilk_hnsw_search(idx, vector, (size_t)limit, &doc_ids, &scores, &count) == 0) {
            res->count = count;
            res->results = calloc(count, sizeof(csilk_vector_search_result_t));
            if (res->results) {
                for (size_t i = 0; i < count; i++) {
                    res->results[i].id = doc_ids[i];
                    res->results[i].score = scores[i];
                }
            }
            free(scores);
            free(doc_ids);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Free a vector DB handle and its underlying driver/index state.
 *
 * If the handle has a driver vtable with a free() callback it is called;
 * otherwise the embedded HNSW index is freed. The handle struct itself is
 * always freed.
 *
 * @param db Database handle to free (may be NULL). */
void
csilk_vector_db_free(csilk_vector_db_t* db)
{
    if (!db) {
        return;
    }
    if (db->driver && db->driver->free) {
        db->driver->free(db->state);
    } else if (db->state) {
        csilk_hnsw_index_free((csilk_hnsw_index_t*)db->state);
    }
    free(db);
}

/**
 * @brief Release all memory owned by a vector search response.
 *
 * Frees each result's id string and payload JSON, the results array, and the
 * error message, then resets the struct to an empty state. Safe to call on a
 * zero-initialised response.
 *
 * @param res Response to clean (may be NULL). */
void
csilk_vector_search_response_free(csilk_vector_search_response_t* res)
{
    if (!res) {
        return;
    }
    if (res->results) {
        for (size_t i = 0; i < res->count; i++) {
            free(res->results[i].id);
            if (res->results[i].payload) {
                csilk_json_free(res->results[i].payload);
            }
        }
        free(res->results);
    }
    free(res->error_message);

    res->results = NULL;
    res->count = 0;
    res->error_message = NULL;
}
