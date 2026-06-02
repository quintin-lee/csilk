#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "csilk/csilk.h"

#define PORT 8899

static volatile int server_ready = 0;

static void
test_handler(csilk_ctx_t* c)
{
	csilk_set_status(c, 200);
	csilk_string(c, 200, "OK");
}

static void
on_server_start(csilk_server_t* s, csilk_ctx_t* c, void* arg)
{
	(void)s;
	(void)c;
	(void)arg;
	server_ready = 1;
}

static void*
run_server(void* arg)
{
	(void)arg;
	csilk_router_t* router = csilk_router_new();

	csilk_route_metadata_t meta = {.input_type = nullptr,
				       .output_type = nullptr,
				       .summary = "Test route",
				       .description = "A route for testing openapi generation"};

	csilk_handler_t h[] = {test_handler};
	CSILK_REGISTER_ROUTE_DOC(router, "GET", "/test", h, 1, meta);

	csilk_server_t* server = csilk_server_new(router);
	csilk_server_config_t config;
	memset(&config, 0, sizeof(config));
	config.enable_openapi = 1;
	config.worker_threads = 1;
	csilk_server_set_config(server, &config);

	csilk_server_add_hook(server, CSILK_HOOK_SERVER_START, on_server_start);

	csilk_server_run(server, PORT);
	return nullptr;
}

static size_t
write_cb(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	char** response = (char**)userdata;
	size_t realsize = size * nmemb;
	*response = realloc(*response, realsize + 1);
	memcpy(*response, ptr, realsize);
	(*response)[realsize] = '\0';
	return realsize;
}

static void
test_openapi_json()
{
	printf("Testing /openapi.json endpoint...\n");
	CURL* curl = curl_easy_init();
	assert(curl != nullptr);

	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/openapi.json", PORT);

	char* response = nullptr;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		printf("curl_easy_perform failed: %s\n", curl_easy_strerror(res));
	}
	assert(res == CURLE_OK);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 200) {
		printf("HTTP code expected 200, got %ld\n", http_code);
		if (response) {
			printf("Response: %s\n", response);
		}
	}
	assert(http_code == 200);

	assert(response != nullptr);
	assert(strstr(response, "\"openapi\"") != nullptr);
	assert(strstr(response, "\"paths\"") != nullptr);
	assert(strstr(response, "/test") != nullptr);
	assert(strstr(response, "Test route") != nullptr);

	free(response);
	curl_easy_cleanup(curl);
	printf("test_openapi.json passed!\n");
}

int
main()
{
	pthread_t tid;
	pthread_create(&tid, nullptr, run_server, nullptr);
	while (!server_ready) {
		usleep(10000);
	}
	sleep(1);

	test_openapi_json();

	printf("All openapi tests passed!\n");
	return 0;
}
