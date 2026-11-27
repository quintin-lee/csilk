#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/drivers/vector.h"

static int mock_init_called = 0;
static int mock_upsert_called = 0;
static int mock_search_called = 0;
static int mock_free_called = 0;

static void*
mock_init(const char* endpoint, const char* api_key)
{
    (void)api_key;
    mock_init_called++;
    char* state = strdup(endpoint);
    return state;
}

static int
mock_upsert(void* state, const char* collection, const csilk_vector_point_t* points, size_t count)
{
    (void)state;
    (void)collection;
    (void)points;
    mock_upsert_called++;
    return (count > 0) ? 0 : -1;
}

static int
mock_search(void*                           state,
            const char*                     collection,
            const float*                    vector,
            size_t                          dimension,
            int                             limit,
            csilk_vector_search_response_t* res)
{
    (void)state;
    (void)collection;
    (void)vector;
    (void)dimension;
    mock_search_called++;

    res->count = 1;
    res->results = calloc(1, sizeof(csilk_vector_search_result_t));
    res->results[0].id = strdup("mock-id-1");
    res->results[0].score = 0.95f;
    res->results[0].payload = nullptr;
    res->error_message = nullptr;
    (void)limit;
    return 0;
}

static void
mock_free(void* state)
{
    mock_free_called++;
    free(state);
}

static csilk_vector_db_driver_t mock_driver = {
    .name = "mock_vector",
    .init = mock_init,
    .upsert = mock_upsert,
    .search = mock_search,
    .free = mock_free,
};

static void
test_vector_register_and_new(void)
{
    printf("Testing vector DB register + new...\n");

    csilk_vector_db_register_driver(&mock_driver);

    csilk_vector_db_t* db = csilk_vector_db_new("mock_vector", "http://localhost:6333", "key");
    assert(db != nullptr);
    assert(mock_init_called == 1);

    csilk_vector_db_free(db);
    assert(mock_free_called == 1);

    printf("  passed\n");
}

static void
test_vector_invalid_driver(void)
{
    printf("Testing vector DB invalid driver...\n");

    csilk_vector_db_t* db = csilk_vector_db_new("nonexistent_driver", "http://localhost", nullptr);
    assert(db == nullptr);

    printf("  passed\n");
}

static void
test_vector_upsert(void)
{
    printf("Testing vector DB upsert...\n");

    csilk_vector_db_t* db = csilk_vector_db_new("mock_vector", "http://localhost:6333", nullptr);
    assert(db != nullptr);

    float                vec[] = {1.0f, 2.0f, 3.0f};
    csilk_vector_point_t point = {
        .id = "pt-1",
        .vector = vec,
        .dimension = 3,
        .payload = nullptr,
    };

    mock_upsert_called = 0;
    int r = csilk_vector_db_upsert(db, "test_collection", &point, 1);
    assert(r == 0);
    assert(mock_upsert_called == 1);

    csilk_vector_db_free(db);
    printf("  passed\n");
}

static void
test_vector_search(void)
{
    printf("Testing vector DB search...\n");

    csilk_vector_db_t* db = csilk_vector_db_new("mock_vector", "http://localhost:6333", nullptr);
    assert(db != nullptr);

    float                          query[] = {1.0f, 2.0f, 3.0f};
    csilk_vector_search_response_t res = {0};

    mock_search_called = 0;
    int r = csilk_vector_db_search(db, "test_collection", query, 3, 10, &res);
    assert(r == 0);
    assert(mock_search_called == 1);
    assert(res.count == 1);
    assert(strcmp(res.results[0].id, "mock-id-1") == 0);
    assert(res.results[0].score > 0.9f);

    csilk_vector_search_response_free(&res);
    csilk_vector_db_free(db);
    printf("  passed\n");
}

static void
test_vector_null_safety(void)
{
    printf("Testing vector DB NULL safety...\n");

    csilk_vector_db_free(nullptr);
    csilk_vector_search_response_free(nullptr);

    csilk_vector_search_response_t res = {0};
    csilk_vector_search_response_free(&res);

    printf("  passed\n");
}

int
main(void)
{
    test_vector_register_and_new();
    test_vector_invalid_driver();
    test_vector_upsert();
    test_vector_search();
    test_vector_null_safety();

    printf("test_vector_db: ALL PASSED\n");
    return 0;
}
