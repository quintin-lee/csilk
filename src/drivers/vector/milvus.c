/**
 * @file milvus.c
 * @brief Milvus Vector Database driver using RESTful API.
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
} milvus_state_t;

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
milvus_init(const char* endpoint, const char* api_key)
{
	if (!endpoint) {
		return nullptr;
	}
	milvus_state_t* state = calloc(1, sizeof(milvus_state_t));
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
milvus_free(void* state_ptr)
{
	milvus_state_t* state = (milvus_state_t*)state_ptr;
	if (!state) {
		return;
	}
	free(state->endpoint);
	free(state->api_key);
	free(state);
}

static int
milvus_upsert(void* state_ptr,
	      const char* collection,
	      const csilk_vector_point_t* points,
	      size_t count)
{
	milvus_state_t* state = (milvus_state_t*)state_ptr;
	CURL* curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	/* Milvus v2.x REST API: POST /v2/vectordb/entities/upsert */
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "collectionName", collection);
	cJSON* data_arr = cJSON_AddArrayToObject(root, "data");
	for (size_t i = 0; i < count; i++) {
		cJSON* p = cJSON_CreateObject();
		/* Milvus usually expects an 'id' field and a 'vector' field */
		cJSON_AddStringToObject(p, "id", points[i].id);

		cJSON* vec_arr = cJSON_AddArrayToObject(p, "vector");
		for (size_t d = 0; d < points[i].dimension; d++) {
			cJSON_AddItemToArray(vec_arr, cJSON_CreateNumber(points[i].vector[d]));
		}
		if (points[i].payload) {
			/* Merge payload fields into the entity object */
			cJSON* field = points[i].payload->child;
			while (field) {
				cJSON_AddItemToObject(p, field->string, cJSON_Duplicate(field, 1));
				field = field->next;
			}
		}
		cJSON_AddItemToArray(data_arr, p);
	}

	char* json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	char url[512];
	snprintf(url, sizeof(url), "%s/v2/vectordb/entities/upsert", state->endpoint);

	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (state->api_key) {
		char auth_hdr[256];
		snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", state->api_key);
		headers = curl_slist_append(headers, auth_hdr);
	}

	struct curl_response cr = {0};
	curl_easy_setopt(curl, CURLOPT_URL, url);
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
milvus_search(void* state_ptr,
	      const char* collection,
	      const float* vector,
	      size_t dimension,
	      int limit,
	      csilk_vector_search_response_t* res)
{
	milvus_state_t* state = (milvus_state_t*)state_ptr;
	CURL* curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	/* Milvus v2.x REST API: POST /v2/vectordb/entities/search */
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "collectionName", collection);
	cJSON* vec_arr = cJSON_AddArrayToObject(root, "vector");
	for (size_t d = 0; d < dimension; d++) {
		cJSON_AddItemToArray(vec_arr, cJSON_CreateNumber(vector[d]));
	}
	cJSON_AddNumberToObject(root, "limit", limit);

	/* Request all fields in output */
	cJSON* output_fields = cJSON_AddArrayToObject(root, "outputFields");
	cJSON_AddItemToArray(output_fields, cJSON_CreateString("*"));

	char* json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	char url[512];
	snprintf(url, sizeof(url), "%s/v2/vectordb/entities/search", state->endpoint);

	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (state->api_key) {
		char auth_hdr[256];
		snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", state->api_key);
		headers = curl_slist_append(headers, auth_hdr);
	}

	struct curl_response cr = {0};
	curl_easy_setopt(curl, CURLOPT_URL, url);
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

	cJSON* data = cJSON_GetObjectItem(resp, "data");
	if (cJSON_IsArray(data)) {
		res->count = cJSON_GetArraySize(data);
		res->results = calloc(res->count, sizeof(csilk_vector_search_result_t));
		for (size_t i = 0; i < res->count; i++) {
			cJSON* item = cJSON_GetArrayItem(data, (int)i);
			cJSON* id = cJSON_GetObjectItem(item, "id");
			if (id) {
				if (cJSON_IsString(id)) {
					res->results[i].id = strdup(id->valuestring);
				} else {
					char buf[64];
					snprintf(buf, sizeof(buf), "%g", id->valuedouble);
					res->results[i].id = strdup(buf);
				}
			}
			cJSON* distance = cJSON_GetObjectItem(item, "distance");
			if (distance) {
				res->results[i].score = (float)distance->valuedouble;
			}

			/* Treat all other fields as payload */
			cJSON* payload = cJSON_CreateObject();
			cJSON* field = item->child;
			while (field) {
				if (strcmp(field->string, "id") != 0 &&
				    strcmp(field->string, "distance") != 0) {
					cJSON_AddItemToObject(
					    payload, field->string, cJSON_Duplicate(field, 1));
				}
				field = field->next;
			}
			res->results[i].payload = payload;
		}
	} else {
		res->count = 0;
		res->results = nullptr;
	}

	cJSON_Delete(resp);
	return 0;
}

const csilk_vector_db_driver_t milvus_driver = {.name = "milvus",
						.init = milvus_init,
						.upsert = milvus_upsert,
						.search = milvus_search,
						.free = milvus_free};

void
csilk_vector_milvus_init(void)
{
	void csilk_vector_db_register_driver(const csilk_vector_db_driver_t* driver);
	csilk_vector_db_register_driver(&milvus_driver);
}
