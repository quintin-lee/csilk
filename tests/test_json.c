#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_bind_json()
{
	printf("Testing csilk_bind_json...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	const char* body = "{\"key\": \"value\"}";
	csilk_test_ctx_set_body(ctx, body, strlen(body));

	cJSON* json = csilk_bind_json(ctx);
	assert(json != NULL);

	cJSON* val = cJSON_GetObjectItem(json, "key");
	assert(val != NULL);
	assert(strcmp(val->valuestring, "value") == 0);

	cJSON_Delete(json);
	csilk_test_ctx_free(ctx);
	printf("csilk_bind_json test passed.\n");
}

void
test_json_response()
{
	printf("Testing csilk_json...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "status", "ok");

	csilk_json(ctx, CSILK_STATUS_OK, json);

	assert(csilk_get_status(ctx) == CSILK_STATUS_OK);
	size_t len;
	const char* body = csilk_get_response_body(ctx, &len);
	assert(body != NULL);
	// Note: csilk_set_header is not implemented for checking, but we rely on
	// csilk_json implementation The body should be "{"status":"ok"}"
	assert(strstr(body, "\"status\":\"ok\"") != NULL);

	csilk_test_ctx_free(ctx);
	printf("csilk_json test passed.\n");
}

void
test_bind_json_err()
{
	printf("Testing csilk_bind_json_err...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();

	const char* err = NULL;
	cJSON* json = csilk_bind_json_err(NULL, &err);
	assert(json == NULL);
	assert(err != NULL);

	json = csilk_bind_json_err(ctx, &err);
	assert(json == NULL);
	assert(err != NULL);
	assert(strcmp(err, "No request body") == 0);

	const char* body1 = "invalid json {{{";
	csilk_test_ctx_set_body(ctx, body1, strlen(body1));
	json = csilk_bind_json_err(ctx, &err);
	assert(json == NULL);
	assert(err != NULL);

	const char* body2 = "{\"valid\": true}";
	csilk_test_ctx_set_body(ctx, body2, strlen(body2));
	json = csilk_bind_json_err(ctx, &err);
	assert(json != NULL);
	assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "valid")));
	cJSON_Delete(json);

	csilk_test_ctx_free(ctx);
	printf("csilk_bind_json_err passed!\n");
}

void
test_json_error()
{
	printf("Testing csilk_json_error...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_json_error(ctx, CSILK_STATUS_NOT_FOUND, "Not found");
	assert(csilk_get_status(ctx) == CSILK_STATUS_NOT_FOUND);
	size_t len;
	const char* body = csilk_get_response_body(ctx, &len);
	assert(body != NULL);
	assert(strstr(body, "\"error\"") != NULL);
	assert(strstr(body, "Not found") != NULL);
	csilk_test_ctx_free(ctx);

	ctx = csilk_test_ctx_new();
	csilk_json_error(ctx, CSILK_STATUS_INTERNAL_SERVER_ERROR, NULL);
	assert(csilk_get_status(ctx) == CSILK_STATUS_INTERNAL_SERVER_ERROR);
	body = csilk_get_response_body(ctx, &len);
	assert(strstr(body, "Unknown error") != NULL);
	csilk_test_ctx_free(ctx);

	csilk_json_error(NULL, CSILK_STATUS_BAD_REQUEST, "should not crash");
	printf("csilk_json_error passed!\n");
}

int
main()
{
	test_bind_json();
	test_json_response();
	test_bind_json_err();
	test_json_error();
	printf("All JSON tests passed!\n");
	return 0;
}
