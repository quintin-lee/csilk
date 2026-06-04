#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/ctx_internal.h"

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

	csilk_ctx_t ctx;
	_csilk_ctx_init(&ctx, NULL, NULL);

	// The fuzzer manually allocated path/query, but csilk_ctx_cleanup expects
	// to own them if assigned. To avoid double-free, we handle them carefully.

	// 2. Fuzz csilk_parse_query
	if (query) {
		// csilk_parse_query might allocate into ctx.params
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
			// Assign path to ctx. request.path takes ownership in some flows,
			// but here we manually manage it to be safe.
			ctx.request.path = path;
			csilk_router_match_ctx(router, &ctx);
			// Reset to NULL so csilk_ctx_cleanup doesn't try to free our manual path
			ctx.request.path = NULL;
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
	if (ctx.arena) {
		csilk_arena_free(ctx.arena);
	}
	free(input);

	return 0;
}
