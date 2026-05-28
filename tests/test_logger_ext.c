#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/csilk.h"

int
main()
{
	csilk_log_config_t cfg = {0};
	cfg.level = CSILK_LOG_DEBUG;

	printf("Testing csilk_log_make_kv...\n");
	{
		cJSON* kv = csilk_log_make_kv("key1", "val1", "key2", "val2", NULL);
		assert(kv != NULL);
		cJSON* v1 = cJSON_GetObjectItem(kv, "key1");
		assert(v1 != NULL && strcmp(v1->valuestring, "val1") == 0);
		cJSON* v2 = cJSON_GetObjectItem(kv, "key2");
		assert(v2 != NULL && strcmp(v2->valuestring, "val2") == 0);
		cJSON_Delete(kv);
	}
	{
		cJSON* kv = csilk_log_make_kv(NULL);
		assert(kv != NULL);
		assert(cJSON_GetObjectItem(kv, "key1") == NULL);
		cJSON_Delete(kv);
	}

	printf("Testing csilk_log_make_kv with odd args (no sentinel)...\n");
	{
		cJSON* kv = csilk_log_make_kv("alone", "val", NULL);
		assert(kv != NULL);
		cJSON* v = cJSON_GetObjectItem(kv, "alone");
		assert(v != NULL);
		cJSON_Delete(kv);
	}

	printf("Testing csilk_log_is_json...\n");
	{
		assert(csilk_log_is_json() == 0);
		cfg.json_format = 1;
		cfg.file_path = NULL;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		assert(csilk_log_is_json() == 1);
		csilk_log_close();
	}

	printf("Testing csilk_log_set_request_id...\n");
	{
		cfg.json_format = 0;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		csilk_log_set_request_id("req-12345");
		csilk_log_close();
		csilk_log_set_request_id(NULL);
	}

	printf("Testing _csilk_log_structured with JSON format...\n");
	{
		cfg.json_format = 1;
		cfg.file_path = NULL;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		cJSON* extra = csilk_log_make_kv("route", "/test", "method", "GET", NULL);
		_csilk_log_structured(
		    CSILK_LOG_INFO, __FILE__, __LINE__, __func__, extra, "structured test message");
		csilk_log_close();
	}

	printf("Testing _csilk_log_structured with text format (extra "
	       "discarded)...\n");
	{
		cfg.json_format = 0;
		cfg.file_path = NULL;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		cJSON* extra = csilk_log_make_kv("route", "/test", NULL);
		_csilk_log_structured(
		    CSILK_LOG_WARN, __FILE__, __LINE__, __func__, extra, "text structured message");
		csilk_log_close();
	}

	printf("Testing _csilk_log_structured with NULL extra...\n");
	{
		cfg.json_format = 1;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		_csilk_log_structured(
		    CSILK_LOG_ERROR, __FILE__, __LINE__, __func__, NULL, "no extra fields");
		csilk_log_close();
	}

	printf("Testing file logging with rotation...\n");
	{
		cfg.json_format = 0;
		cfg.file_path = "test_logger_rotation.log";
		cfg.max_file_size = 100;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		for (int i = 0; i < 50; i++) {
			CSILK_LOG_I("test rotation line %d", i);
		}
		csilk_log_close();
		remove("test_logger_rotation.log");
		remove("test_logger_rotation.log.1");
	}

	printf("Testing CSILK_LOG_KV macro...\n");
	{
		cfg.json_format = 0;
		cfg.file_path = NULL;
		int ret = csilk_log_init(cfg);
		assert(ret == 0);
		cJSON* extra = csilk_log_make_kv("test", "macro", NULL);
		CSILK_LOG_STRUCT(CSILK_LOG_DEBUG, extra, "CSILK_LOG_STRUCT test message");
		csilk_log_close();
	}

	printf("test_logger_ext: PASS\n");
	return 0;
}
