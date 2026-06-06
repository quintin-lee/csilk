#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

// Mock handler for middleware testing
static int handler_called = 0;
void
mock_handler(csilk_ctx_t* c)
{
	(void)c;
	handler_called++;
}

// Mock Permission Driver
static int driver_eval_called = 0;
static int
mock_eval(csilk_ctx_t* c, const char* perm, const char* res)
{
	(void)c;
	driver_eval_called++;
	const char* role = (const char*)csilk_get(c, "role");

	if (role && strcmp(role, "admin") == 0) {
		return 0; // 0 = allowed
	}
	if (perm && strcmp(perm, "read") == 0 && res && strcmp(res, "public") == 0) {
		return 0; // 0 = allowed
	}
	return -1; // non-zero = denied
}

static csilk_perm_driver_t mock_driver = {.name = "mock", .check = mock_eval};

void
test_perm_simple_no_role()
{
	printf("Testing simple perm (No role)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	// Default simple driver
	csilk_perm_simple_init();
	csilk_perm_set_default("simple");

	// No role set in context, should deny
	csilk_perm_require(c, "read", "users:1");
	assert(csilk_is_aborted(c) == 1);
	assert(csilk_get_status(c) == CSILK_STATUS_FORBIDDEN);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_no_role passed\n");
}

void
test_perm_simple_role_from_csilk_get()
{
	printf("Testing simple perm (Role from csilk_get)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_set(c, "role", "user");
	csilk_perm_simple_allow("user", "read", "articles:*");

	csilk_perm_require(c, "read", "articles:42");
	assert(csilk_is_aborted(c) == 0);

	csilk_perm_require(c, "write", "articles:42");
	assert(csilk_is_aborted(c) == 1);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_role_from_csilk_get passed\n");
}

void
test_perm_simple_role_from_jwt()
{
	printf("Testing simple perm (Role from JWT)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	cJSON* jwt = cJSON_CreateObject();
	cJSON_AddStringToObject(jwt, "role", "editor");
	csilk_set(c, "jwt_payload", jwt);

	csilk_perm_simple_allow("editor", "edit", "*");
	csilk_perm_require(c, "edit", "any");
	assert(csilk_is_aborted(c) == 0);

	cJSON_Delete(jwt);
	csilk_test_ctx_free(c);
	printf("test_perm_simple_role_from_jwt passed\n");
}

void
test_perm_simple_wildcard_permission()
{
	printf("Testing simple perm (Wildcard permission)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "admin");

	csilk_perm_simple_allow("admin", "*", "*");
	csilk_perm_require(c, "anything", "anywhere");
	assert(csilk_is_aborted(c) == 0);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_wildcard_permission passed\n");
}

void
test_perm_simple_wildcard_role()
{
	printf("Testing simple perm (Wildcard role)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "guest");

	csilk_perm_simple_allow("*", "view", "landing");
	csilk_perm_require(c, "view", "landing");
	assert(csilk_is_aborted(c) == 0);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_wildcard_role passed\n");
}

void
test_perm_simple_deny_no_rule()
{
	printf("Testing simple perm (Deny when no rule matches)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "user");

	csilk_perm_require(c, "delete", "server");
	assert(csilk_is_aborted(c) == 1);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_deny_no_rule passed\n");
}

void
test_perm_simple_exact_match()
{
	printf("Testing simple perm (Exact match)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "user");

	csilk_perm_simple_allow("user", "read", "secret");
	csilk_perm_require(c, "read", "secret");
	assert(csilk_is_aborted(c) == 0);

	csilk_perm_require(c, "read", "secret2");
	assert(csilk_is_aborted(c) == 1);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_exact_match passed\n");
}

void
test_perm_simple_prefix_resource()
{
	printf("Testing simple perm (Prefix resource articles:*)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "user");

	csilk_perm_simple_allow("user", "read", "articles:*");
	csilk_perm_require(c, "read", "articles:123");
	assert(csilk_is_aborted(c) == 0);

	csilk_perm_require(c, "read", "users:123");
	assert(csilk_is_aborted(c) == 1);

	csilk_test_ctx_free(c);
	printf("test_perm_simple_prefix_resource passed\n");
}

void
test_perm_custom_driver()
{
	printf("Testing custom perm driver...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_perm_register_driver("mock", &mock_driver);
	csilk_perm_set_default("mock");

	csilk_set(c, "role", "admin");
	driver_eval_called = 0;
	csilk_perm_require(c, "any", "any");
	assert(driver_eval_called == 1);
	assert(csilk_is_aborted(c) == 0);

	csilk_set(c, "role", "guest");
	csilk_perm_require(c, "read", "public");
	assert(csilk_is_aborted(c) == 0);

	csilk_perm_require(c, "read", "private");
	assert(csilk_is_aborted(c) == 1);

	csilk_test_ctx_free(c);
	printf("test_perm_custom_driver passed\n");
}

void
test_perm_router_route_perm()
{
	printf("Testing router add_route_perm...\n");
	csilk_router_t* r = csilk_router_new();
	csilk_handler_t h[] = {mock_handler};

	csilk_router_add_perm(r, "GET", "/test", h, 1, "read", "resource");

	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_test_ctx_set_request(c, "GET", "/test");

	int matched = csilk_router_match_ctx(r, c);
	assert(matched);
	assert(strcmp(csilk_ctx_get_handler_perm_required(c), "read") == 0);
	assert(strcmp(csilk_ctx_get_handler_perm_resource(c), "resource") == 0);

	csilk_test_ctx_free(c);
	csilk_router_free(r);
	printf("test_perm_router_route_perm passed\n");
}

void
test_perm_router_route_extended_perm()
{
	printf("Testing router add_route_extended_perm...\n");
	csilk_router_t* r = csilk_router_new();
	csilk_handler_t h[] = {mock_handler};

	csilk_router_add_extended_perm(r,
				       "GET",
				       "/test",
				       h,
				       1,
				       "/test",
				       nullptr,
				       nullptr,
				       nullptr,
				       nullptr,
				       "read",
				       "resource");

	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_test_ctx_set_request(c, "GET", "/test");

	int matched = csilk_router_match_ctx(r, c);
	assert(matched);
	assert(strcmp(csilk_ctx_get_handler_perm_required(c), "read") == 0);
	assert(strcmp(csilk_ctx_get_handler_perm_resource(c), "resource") == 0);

	csilk_test_ctx_free(c);
	csilk_router_free(r);
	printf("test_perm_router_route_extended_perm passed\n");
}

void
test_perm_auto_middleware_allowed()
{
	printf("Testing csilk_perm_auto_middleware (Allowed)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "admin");
	csilk_perm_simple_init();
	csilk_perm_set_default("simple");
	csilk_perm_simple_allow("admin", "write", "*");

	csilk_test_ctx_set_handler_metadata(c, "write", "articles:42");

	csilk_perm_auto_middleware(c);
	assert(csilk_is_aborted(c) == 0);

	csilk_test_ctx_free(c);
	printf("test_perm_auto_middleware_allowed passed\n");
}

void
test_perm_auto_middleware_denied()
{
	printf("Testing csilk_perm_auto_middleware (Denied)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set(c, "role", "user");
	csilk_perm_simple_init();
	csilk_perm_set_default("simple");

	csilk_test_ctx_set_handler_metadata(c, "delete", "users:1");

	csilk_perm_auto_middleware(c);
	assert(csilk_is_aborted(c) == 1);
	assert(csilk_get_status(c) == CSILK_STATUS_FORBIDDEN);

	csilk_test_ctx_free(c);
	printf("test_perm_auto_middleware_denied passed\n");
}

void
test_perm_auto_middleware_no_perm()
{
	printf("Testing csilk_perm_auto_middleware (No perm required)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_test_ctx_set_handler_metadata(c, nullptr, nullptr);

	csilk_perm_auto_middleware(c);
	assert(csilk_is_aborted(c) == 0);

	csilk_test_ctx_free(c);
	printf("test_perm_auto_middleware_no_perm passed\n");
}

void
test_perm_auto_middleware_no_handler()
{
	printf("Testing csilk_perm_auto_middleware (No handler)...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_perm_auto_middleware(c);
	assert(csilk_is_aborted(c) == 0);

	csilk_test_ctx_free(c);
	printf("test_perm_auto_middleware_no_handler passed\n");
}

int
main()
{
	csilk_perm_init();

	test_perm_simple_no_role();
	test_perm_simple_role_from_csilk_get();
	test_perm_simple_role_from_jwt();
	test_perm_simple_wildcard_permission();
	test_perm_simple_wildcard_role();
	test_perm_simple_deny_no_rule();
	test_perm_simple_exact_match();
	test_perm_simple_prefix_resource();
	test_perm_custom_driver();
	test_perm_router_route_perm();
	test_perm_router_route_extended_perm();
	test_perm_auto_middleware_allowed();
	test_perm_auto_middleware_denied();
	test_perm_auto_middleware_no_perm();
	test_perm_auto_middleware_no_handler();

	printf("test_perm: ALL PASSED\n");
	return 0;
}
