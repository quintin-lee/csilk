/**
 * @file qdrant.c
 * @brief Qdrant Vector Database driver.
 * @copyright MIT License
 */

#include "csilk/drivers/vector.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct {
	char* endpoint;
	char* api_key;
} qdrant_state_t;

struct curl_response {
	char* body;
	size_t size;
};

static size_t
write_cb_simple(void* contents, size_t size, size_t nmemb, void* userp)
{
	size_t realsize = size * nmemb;
	struct curl_response* res = (struct curl_response*)userp;
	char* ptr = realloc(res->body, res->size + realsize + 1);
	if (!ptr) {
		return 0;
	}
	res->body = ptr;
	memcpy(&(res->body[res->size]), contents, realsize);
	res->size += realsize;
	res->body[res->size] = 0;
	return realsize;
}

static void*
qdrant_init(const char* endpoint, const char* api_key)
{
	if (!endpoint) {
		return nullptr;
	}
	qdrant_state_t* state = calloc(1, sizeof(qdrant_state_t));
	if (!state) {
		return nullptr;
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

static int
qdrant_upsert(void* state_ptr,
	      const char* collection,
	      const csilk_vector_point_t* points,
	      size_t count)
{
	qdrant_state_t* state = (qdrant_state_t*)state_ptr;
	CURL* curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	cJSON* root = cJSON_CreateObject();
	cJSON* points_arr = cJSON_AddArrayToObject(root, "points");
	for (size_t i = 0; i < count; i++) {
		cJSON* p = cJSON_CreateObject();
		cJSON_AddStringToObject(p, "id", points[i].id);

		cJSON* vec_arr = cJSON_AddArrayToObject(p, "vector");
		for (size_t d = 0; d < points[i].dimension; d++) {
			cJSON_AddItemToArray(vec_arr, cJSON_CreateNumber(points[i].vector[d]));
		}
		if (points[i].payload) {
			cJSON_AddItemToObject(p, "payload", cJSON_Duplicate(points[i].payload, 1));
		}
		cJSON_AddItemToArray(points_arr, p);
	}

	char* json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	char url[512];
	snprintf(url, sizeof(url), "%s/collections/%s/points", state->endpoint, collection);

	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (state->api_key) {
		char auth_hdr[256];
		snprintf(auth_hdr, sizeof(auth_hdr), "api-key: %s", state->api_key);
		headers = curl_slist_append(headers, auth_hdr);
	}

	struct curl_response cr = {0};
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb_simple);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);

	CURLcode rc = curl_easy_perform(curl);
	free(json_body);
	curl_slist_free_all(headers);

	int ret = 0;
	if (rc != CURLE_OK) {
		ret = -1;
	} else {
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code >= 400) {
			ret = -1;
		}
	}

	free(cr.body);
	curl_easy_cleanup(curl);
	return ret;
}

static int
qdrant_search(void* state_ptr,
	      const char* collection,
	      const float* vector,
	      size_t dimension,
	      int limit,
	      csilk_vector_search_response_t* res)
{
	qdrant_state_t* state = (qdrant_state_t*)state_ptr;
	CURL* curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	cJSON* root = cJSON_CreateObject();
	cJSON* vec_arr = cJSON_AddArrayToObject(root, "vector");
	for (size_t d = 0; d < dimension; d++) {
		cJSON_AddItemToArray(vec_arr, cJSON_CreateNumber(vector[d]));
	}
	cJSON_AddNumberToObject(root, "limit", limit);
	cJSON_AddBoolToObject(root, "with_payload", 1);

	char* json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	char url[512];
	snprintf(url, sizeof(url), "%s/collections/%s/points/search", state->endpoint, collection);

	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (state->api_key) {
		char auth_hdr[256];
		snprintf(auth_hdr, sizeof(auth_hdr), "api-key: %s", state->api_key);
		headers = curl_slist_append(headers, auth_hdr);
	}

	struct curl_response cr = {0};
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb_simple);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);

	CURLcode rc = curl_easy_perform(curl);
	free(json_body);
	curl_slist_free_all(headers);

	if (rc != CURLE_OK) {
		res->error_message = strdup(curl_easy_strerror(rc));
		free(cr.body);
		curl_easy_cleanup(curl);
		return -1;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code >= 400) {
		res->error_message = cr.body ? strdup(cr.body) : strdup("HTTP error");
		free(cr.body);
		curl_easy_cleanup(curl);
		return -1;
	}

	cJSON* resp = cJSON_Parse(cr.body);
	free(cr.body);
	curl_easy_cleanup(curl);

	if (!resp) {
		res->error_message = strdup("JSON parse error");
		return -1;
	}

	cJSON* result_arr = cJSON_GetObjectItem(resp, "result");
	if (cJSON_IsArray(result_arr)) {
		res->count = cJSON_GetArraySize(result_arr);
		res->results = calloc(res->count, sizeof(csilk_vector_search_result_t));
		for (size_t i = 0; i < res->count; i++) {
			cJSON* item = cJSON_GetArrayItem(result_arr, (int)i);
			cJSON* id = cJSON_GetObjectItem(item, "id");
			if (id) {
				if (cJSON_IsString(id)) {
					res->results[i].id = strdup(id->valuestring);
				} else if (cJSON_IsNumber(id)) {
					char buf[32];
					snprintf(buf, sizeof(buf), "%d", id->valueint);
					res->results[i].id = strdup(buf);
				}
			}
			cJSON* score = cJSON_GetObjectItem(item, "score");
			if (score) {
				res->results[i].score = (float)score->valuedouble;
			}

			cJSON* payload = cJSON_GetObjectItem(item, "payload");
			if (payload) {
				res->results[i].payload = cJSON_Duplicate(payload, 1);
			}
		}
	} else {
		res->count = 0;
		res->results = nullptr;
	}

	cJSON_Delete(resp);
	return 0;
}

const csilk_vector_db_driver_t qdrant_driver = {.name = "qdrant",
						.init = qdrant_init,
						.upsert = qdrant_upsert,
						.search = qdrant_search,
						.free = qdrant_free};

void
csilk_vector_qdrant_init(void)
{
	void csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver);
	csilk_vector_db_register_driver(&qdrant_driver);
}
