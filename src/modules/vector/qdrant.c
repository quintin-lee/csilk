/**
 * @file qdrant.c
 * @brief Qdrant Vector Database driver.
 * @copyright MIT License
 */

#include "csilk/drivers/vector.h"
#include "modules/common/curl_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "csilk/core/json/json.h"

typedef struct {
    char* endpoint;
    char* api_key;
} qdrant_state_t;

/**
 * @brief Initialise a Qdrant driver state from an endpoint and API key.
 *
 * Stores the endpoint and optional API key (deep-copied) and performs
 * one-shot libcurl global init. Returns NULL if no endpoint is supplied or
 * allocation fails.
 *
 * @param endpoint Base URL of the Qdrant REST API (required).
 * @param api_key  Optional API key sent as the "api-key" header (may be NULL).
 * @return Heap-allocated state pointer, or NULL on failure. */
static void*
qdrant_init(const char* endpoint, const char* api_key)
{
    if (!endpoint) {
        return NULL;
    }
    qdrant_state_t* state = calloc(1, sizeof(qdrant_state_t));
    if (!state) {
        return NULL;
    }
    state->endpoint = strdup(endpoint);
    if (api_key) {
        state->api_key = strdup(api_key);
    }

    static int curl_init = 0;
    if (!curl_init) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_init = 1;
    }
    return state;
}

/**
 * @brief Free Qdrant driver state and its endpoint/api-key strings.
 *
 * @param state_ptr Driver state returned by qdrant_init() (may be NULL). */
static void
qdrant_free(void* state_ptr)
{
    qdrant_state_t* state = (qdrant_state_t*)state_ptr;
    if (!state) {
        return;
    }
    free(state->endpoint);
    free(state->api_key);
    free(state);
}

/**
 * @brief Upsert vectors into a Qdrant collection via the REST API.
 *
 * Serialises each point (id, vector, and optional payload) as a JSON entity
 * and PUTs them to /collections/{collection}/points. A NULL API key omits the
 * "api-key" header. Returns 0 on HTTP < 400 success, -1 on transport error or
 * server error.
 *
 * @param state_ptr  Driver state (must be a qdrant_state_t).
 * @param collection Target collection name.
 * @param points     Array of vectors to upsert.
 * @param count      Number of points.
 * @return 0 on success, -1 on transport or HTTP error. */
static int
qdrant_upsert(void*                       state_ptr,
              const char*                 collection,
              const csilk_vector_point_t* points,
              size_t                      count)
{
    qdrant_state_t* state = (qdrant_state_t*)state_ptr;
    if (!state || !state->endpoint) {
        return -1;
    }

    csilk_json_t* root = csilk_json_object();
    csilk_json_t* points_arr = csilk_json_add_array_obj(root, "points", csilk_json_array());
    for (size_t i = 0; i < count; i++) {
        csilk_json_t* p = csilk_json_object();
        csilk_json_add_string(p, "id", points[i].id);

        csilk_json_t* vec_arr = csilk_json_add_array_obj(p, "vector", csilk_json_array());
        for (size_t d = 0; d < points[i].dimension; d++) {
            csilk_json_array_append(vec_arr, csilk_json_number(points[i].vector[d]));
        }
        if (points[i].payload) {
            csilk_json_add_object(p, "payload", csilk_json_copy(points[i].payload));
        }
        csilk_json_array_append(points_arr, p);
    }

    char* json_body = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    if (!json_body) {
        return -1;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/collections/%s/points", state->endpoint, collection);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (state->api_key) {
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "api-key: %s", state->api_key);
        headers = curl_slist_append(headers, auth_hdr);
    }

    csilk_curl_buf_t cr = {0};
    long             http_code = 0;
    int              rc = csilk_curl_request_json(
        "PUT", url, headers, json_body, 10000L, 30000L, &cr, &http_code, NULL, 0);
    free(json_body);
    curl_slist_free_all(headers);
    free(cr.data);

    return (rc == 0 && http_code < 400) ? 0 : -1;
}

/**
 * @brief Search a Qdrant collection via the REST API.
 *
 * POSTs the query vector, limit and with_payload=1 to
 * /collections/{collection}/points/search. On success the "result" array is
 * converted into csilk_vector_search_result_t entries (id, score, and payload
 * JSON). On failure an error message is set.
 *
 * @param state_ptr  Driver state (must be a qdrant_state_t).
 * @param collection Target collection name.
 * @param vector     Query vector.
 * @param dimension  Dimensionality of @p vector.
 * @param limit      Maximum number of results.
 * @param[out] res   Response struct to populate.
 * @return 0 on success, -1 on transport/HTTP/parse error. */
static int
qdrant_search(void*                           state_ptr,
              const char*                     collection,
              const float*                    vector,
              size_t                          dimension,
              int                             limit,
              csilk_vector_search_response_t* res)
{
    qdrant_state_t* state = (qdrant_state_t*)state_ptr;
    if (!state || !state->endpoint) {
        return -1;
    }

    csilk_json_t* root = csilk_json_object();
    csilk_json_t* vec_arr = csilk_json_add_array_obj(root, "vector", csilk_json_array());
    for (size_t d = 0; d < dimension; d++) {
        csilk_json_array_append(vec_arr, csilk_json_number(vector[d]));
    }
    csilk_json_add_number(root, "limit", limit);
    csilk_json_add_bool(root, "with_payload", 1);

    char* json_body = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    if (!json_body) {
        return -1;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/collections/%s/points/search", state->endpoint, collection);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (state->api_key) {
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "api-key: %s", state->api_key);
        headers = curl_slist_append(headers, auth_hdr);
    }

    csilk_curl_buf_t cr = {0};
    long             http_code = 0;
    char             err_buf[256] = {0};
    int              rc = csilk_curl_request_json(
        "POST", url, headers, json_body, 10000L, 30000L, &cr, &http_code, err_buf, sizeof(err_buf));
    free(json_body);
    curl_slist_free_all(headers);

    if (rc != 0) {
        res->error_message = strdup(err_buf[0] ? err_buf : "CURL error");
        free(cr.data);
        return -1;
    }

    if (http_code >= 400) {
        res->error_message = cr.data ? strdup(cr.data) : strdup("HTTP error");
        free(cr.data);
        return -1;
    }

    csilk_json_t* resp = csilk_json_parse(cr.data);
    free(cr.data);

    if (!resp) {
        res->error_message = strdup("JSON parse error");
        return -1;
    }

    csilk_json_t* result_arr = csilk_json_get(resp, "result");
    if (csilk_json_is_array(result_arr)) {
        res->count = csilk_json_array_size(result_arr);
        res->results = calloc(res->count, sizeof(csilk_vector_search_result_t));
        for (size_t i = 0; i < res->count; i++) {
            csilk_json_t* item = csilk_json_array_get(result_arr, (int)i);
            csilk_json_t* id = csilk_json_get(item, "id");
            if (id) {
                if (csilk_json_is_string(id)) {
                    res->results[i].id = strdup(csilk_json_string_value(id));
                } else if (csilk_json_is_number(id)) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%lld", (long long)csilk_json_int_value(id));
                    res->results[i].id = strdup(buf);
                }
            }
            csilk_json_t* score = csilk_json_get(item, "score");
            if (score) {
                res->results[i].score = (float)csilk_json_number_value(score);
            }

            csilk_json_t* payload = csilk_json_get(item, "payload");
            if (payload) {
                res->results[i].payload = csilk_json_copy(payload);
            }
        }
    } else {
        res->count = 0;
        res->results = NULL;
    }

    csilk_json_free(resp);
    return 0;
}

/**
 * @brief Register the Qdrant vector-DB driver with the subsystem.
 *
 * Adds the qdrant_driver vtable to the global vector driver registry so it can
 * be selected by name from csilk_vector_db_new().
 *
 * @note Idempotency is not enforced here; registering twice would add a
 *       second entry. */
void
csilk_vector_qdrant_init(void)
{
    static const csilk_vector_db_driver_t qdrant_driver = {.name = "qdrant",
                                                           .init = qdrant_init,
                                                           .upsert = qdrant_upsert,
                                                           .search = qdrant_search,
                                                           .free = qdrant_free};
    void csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver);
    csilk_vector_db_register_driver(&qdrant_driver);
}
