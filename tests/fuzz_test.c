#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

// Fuzz test for csilk_split_url, csilk_parse_query, and routing

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size == 0) {
		return 0;
	}

	// Create null-terminated string
	char* input = malloc(size + 1);
	if (!input) {
		return 0;
	}
	memcpy(input, data, size);
	input[size] = '\0';

	// 1. Fuzz csilk_split_url
	char* path = NULL;
	char* query = NULL;
	csilk_split_url(input, &path, &query);

	csilk_ctx_t ctx = {0};

	// 2. Fuzz csilk_parse_query
	if (query) {
		csilk_parse_query(&ctx, query);
	}

	// 3. Fuzz routing
	csilk_router_t* router = csilk_router_new();
	if (router) {
		csilk_handler_t handlers[] = {NULL};
		csilk_router_add(router, "GET", "/api/:id/users/*action", handlers, 1);
		csilk_router_add(router, "POST", "/api/ping", handlers, 1);

		if (path) {
			ctx.request.method = "GET";
			ctx.request.path = path;
			csilk_router_match_ctx(router, &ctx);
		}
		csilk_router_free(router);
	}

	if (path) {
		free(path);
	}
	if (query) {
		free(query);
	}
	csilk_ctx_cleanup(&ctx);
	free(input);

	return 0;
}
