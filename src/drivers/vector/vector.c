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
static int vector_driver_count = 0;

struct csilk_vector_db_s {
	const csilk_vector_db_driver_t* driver;
	void* state;
};

void
csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver)
{
	if (vector_driver_count < MAX_VECTOR_DRIVERS) {
		vector_drivers[vector_driver_count++] = driver;
	}
}

extern void csilk_vector_qdrant_init(void);
extern void csilk_vector_milvus_init(void);

csilk_vector_db_t*
csilk_vector_db_new(const char* driver_name, const char* endpoint, const char* api_key)
{
	static int initialized = 0;
	if (!initialized) {
		csilk_vector_qdrant_init();
		csilk_vector_milvus_init();
		initialized = 1;
	}

	const csilk_vector_db_driver_t* driver = nullptr;
	for (int i = 0; i < vector_driver_count; i++) {
		if (strcmp(vector_drivers[i]->name, driver_name) == 0) {
			driver = vector_drivers[i];
			break;
		}
	}

	if (!driver) {
		return nullptr;
	}

	void* state = driver->init(endpoint, api_key);
	if (!state) {
		return nullptr;
	}

	csilk_vector_db_t* db = malloc(sizeof(csilk_vector_db_t));
	if (!db) {
		driver->free(state);
		return nullptr;
	}

	db->driver = driver;
	db->state = state;
	return db;
}

int
csilk_vector_db_upsert(csilk_vector_db_t* db,
		       const char* collection,
		       const csilk_vector_point_t* points,
		       size_t count)
{
	if (!db || !collection || !points) {
		return -1;
	}
	return db->driver->upsert(db->state, collection, points, count);
}

int
csilk_vector_db_search(csilk_vector_db_t* db,
		       const char* collection,
		       const float* vector,
		       size_t dimension,
		       int limit,
		       csilk_vector_search_response_t* res)
{
	if (!db || !collection || !vector || !res) {
		return -1;
	}
	return db->driver->search(db->state, collection, vector, dimension, limit, res);
}

void
csilk_vector_db_free(csilk_vector_db_t* db)
{
	if (!db) {
		return;
	}
	db->driver->free(db->state);
	free(db);
}

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
				cJSON_Delete(res->results[i].payload);
			}
		}
		free(res->results);
	}
	free(res->error_message);

	res->results = nullptr;
	res->count = 0;
	res->error_message = nullptr;
}
