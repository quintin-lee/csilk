#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/app.h"
#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"

static int mid_called = 0;
static int hnd_called = 0;

static void
mid_handler(csilk_ctx_t* c)
{
	mid_called++;
	csilk_next(c);
}

static void
hello_handler(csilk_ctx_t* c)
{
	hnd_called++;
	csilk_string(c, CSILK_STATUS_OK, "hello");
}

int
main()
{
	printf("Testing csilk_app_new with NULL config...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_log_level...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_log_level(app, CSILK_LOG_DEBUG);
		csilk_app_log_level(app, CSILK_LOG_INFO);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_log_json...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_log_json(app, 1);
		csilk_app_log_json(app, 0);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_log_file...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_log_file(app, "test_app_ext.log", 1024);
		csilk_app_log_file(app, NULL, 0);
		csilk_app_free(app);
		remove("test_app_ext.log");
	}

	printf("Testing csilk_app_add_handlers...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_handler_t handlers[] = {hello_handler, NULL};
		csilk_app_add_handlers(app, "GET", "/handle", handlers, 1);
		csilk_ctx_t ctx = {0};
		ctx.request.method = "GET";
		ctx.request.path = strdup("/handle");
		csilk_router_t* router = csilk_app_router(app);
		assert(router != NULL);
		int matched = csilk_router_match_ctx(router, &ctx);
		assert(matched);
		hnd_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(hnd_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_add_route_extended...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_add_route_extended(app,
					     "GET",
					     "/extended",
					     hello_handler,
					     "Input",
					     "Output",
					     "Summary",
					     "Description");
		csilk_ctx_t ctx = {0};
		ctx.request.method = "GET";
		ctx.request.path = strdup("/extended");
		csilk_router_t* router = csilk_app_router(app);
		assert(router != NULL);
		int matched = csilk_router_match_ctx(router, &ctx);
		assert(matched);
		hnd_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(hnd_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_add_route_extended_perm...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_add_route_extended_perm(app,
						  "DELETE",
						  "/extperm/:id",
						  hello_handler,
						  NULL,
						  NULL,
						  "Del",
						  "Delete",
						  "admin",
						  "users");
		csilk_ctx_t ctx = {0};
		ctx.request.method = "DELETE";
		ctx.request.path = strdup("/extperm/42");
		csilk_router_t* router = csilk_app_router(app);
		assert(router != NULL);
		int matched = csilk_router_match_ctx(router, &ctx);
		assert(matched);
		hnd_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(hnd_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_add_route_perm...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_add_route_perm(app, "GET", "/perm", hello_handler, "read", "documents");
		csilk_ctx_t ctx = {0};
		ctx.request.method = "GET";
		ctx.request.path = strdup("/perm");
		csilk_router_t* router = csilk_app_router(app);
		assert(router != NULL);
		int matched = csilk_router_match_ctx(router, &ctx);
		assert(matched);
		hnd_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(hnd_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_use (server-level middleware)...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_use(app, mid_handler);
		csilk_app_add_route(app, "GET", "/midtest", hello_handler);
		csilk_server_t* srv = csilk_app_server(app);
		assert(srv != NULL);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_use_group...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_use_group(app, "/api", mid_handler);
		csilk_app_add_route(app, "GET", "/api/test", hello_handler);
		csilk_ctx_t ctx = {0};
		ctx.request.method = "GET";
		ctx.request.path = strdup("/api/test");
		csilk_router_t* router = csilk_app_router(app);
		assert(router != NULL);
		int matched = csilk_router_match_ctx(router, &ctx);
		assert(matched);
		hnd_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(hnd_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_apply_config...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_config_t* before = csilk_app_config(app);
		assert(before != NULL);
		csilk_app_apply_config(app);
		free(before);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_enable_openapi...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_app_enable_openapi(app, 0);
		csilk_app_enable_openapi(app, 1);
		csilk_app_free(app);
	}

	printf("Testing csilk_app_set_server_config...\n");
	{
		csilk_app_t* app = csilk_app_new(NULL);
		assert(app != NULL);
		csilk_server_config_t scfg = {0};
		scfg.idle_timeout_ms = 9999;
		scfg.max_body_size = 65536;
		csilk_app_set_server_config(app, scfg);
		csilk_app_free(app);
	}

	printf("Testing csilk_app NULL safety...\n");
	{
		csilk_app_add_route(NULL, "GET", "/nope", hello_handler);
		csilk_app_add_handlers(NULL, "GET", "/nope", NULL, 0);
		csilk_app_add_route_perm(NULL, "GET", "/nope", hello_handler, NULL, NULL);
		csilk_app_add_route_extended(
		    NULL, "GET", "/nope", hello_handler, NULL, NULL, NULL, NULL);
		csilk_app_add_route_extended_perm(
		    NULL, "GET", "/nope", hello_handler, NULL, NULL, NULL, NULL, NULL, NULL);
		csilk_app_use(NULL, mid_handler);
		csilk_app_use_group(NULL, "/api", mid_handler);
		csilk_app_apply_config(NULL);
		csilk_app_enable_openapi(NULL, 0);
		csilk_app_log_level(NULL, CSILK_LOG_INFO);
		csilk_app_log_json(NULL, 0);
		csilk_app_log_file(NULL, NULL, 0);
		{
			csilk_server_config_t z = {0};
			csilk_app_set_server_config(NULL, z);
		}
		csilk_config_t* cp = csilk_app_config(NULL);
		assert(cp == NULL);
	}

	printf("test_app_ext: PASS\n");
	return 0;
}
