/**
 * @file vector.c
 * @brief Core registry and implementations for Vector DB drivers.
 * @copyright MIT License
 */

#include "csilk/drivers/vector.h"
#include <stdlib.h>
#include <string.h>

#define MAX_VECTOR_DRIVERS 4

static const csilk_vector_db_driver_t* vector_drivers[MAX_VECTOR_DRIVERS];
static int vector_driver_count = 0;

void
csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver)
{
	if (vector_driver_count < MAX_VECTOR_DRIVERS) {
		vector_drivers[vector_driver_count++] = driver;
	}
}

const csilk_vector_db_driver_t*
csilk_vector_db_get_driver(const char* name)
{
	for (int i = 0; i < vector_driver_count; i++) {
		if (strcmp(vector_drivers[i]->name, name) == 0) {
			return vector_drivers[i];
		}
	}
	return nullptr;
}

void
csilk_vector_search_response_free(csilk_vector_search_response_t* res)
{
	if (!res || !res->results) {
		return;
	}
	for (size_t i = 0; i < res->count; i++) {
		free(res->results[i].id);
		if (res->results[i].payload) {
			cJSON_Delete(res->results[i].payload);
		}
	}
	free(res->results);
	free(res->error_message);

	res->results = nullptr;
	res->count = 0;
	res->error_message = nullptr;
}
