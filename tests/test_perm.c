#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/drivers/perm.h"

static int custom_check_called = 0;

static int custom_check(csilk_ctx_t* c, const char* permission,
                        const char* resource) {
  (void)c;
  (void)permission;
  (void)resource;
  custom_check_called++;
  return 0;
}

static csilk_perm_driver_t custom_driver = {
    .name = "custom",
    .check = custom_check,
};

void test_perm_init(void) {
  csilk_perm_init();
  csilk_perm_driver_t* d = csilk_perm_get_driver("simple");
  assert(d != NULL);
  assert(strcmp(d->name, "simple") == 0);
  printf("test_perm_init passed\n");
}

void test_perm_register_lookup(void) {
  assert(csilk_perm_register_driver("custom2", &custom_driver) == 0);
  csilk_perm_driver_t* d = csilk_perm_get_driver("custom2");
  assert(d != NULL);
  assert(strcmp(d->name, "custom2") == 0);
  assert(csilk_perm_get_driver("nonexistent") == NULL);
  printf("test_perm_register_lookup passed\n");
}

void test_perm_set_default(void) {
  csilk_perm_set_default("simple");
  csilk_perm_driver_t* d = csilk_perm_get_driver("simple");
  assert(d != NULL);
  assert(csilk_perm_set_default("nonexistent") == -1);
  printf("test_perm_set_default passed\n");
}

static void reset_simple(void) {
  csilk_perm_set_default("simple");
  csilk_perm_simple_clear();
}

void test_perm_simple_no_role(void) {
  reset_simple();

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));

  int rc = csilk_perm_check(&c, "read", "articles:1");
  assert(rc == -1);
  printf("test_perm_simple_no_role passed\n");
}

void test_perm_simple_role_from_csilk_get(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"admin");
  csilk_perm_simple_allow("admin", "read", "*");

  int rc = csilk_perm_check(&c, "read", "anything");
  assert(rc == 0);

  csilk_arena_free(c.arena);
  printf("test_perm_simple_role_from_csilk_get passed\n");
}

void test_perm_simple_role_from_jwt(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  cJSON* payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "role", "editor");
  csilk_set(&c, "jwt_payload", (void*)payload);

  csilk_perm_simple_allow("editor", "write", "articles:*");

  int rc = csilk_perm_check(&c, "write", "articles:42");
  assert(rc == 0);

  cJSON_Delete(payload);
  csilk_arena_free(c.arena);
  printf("test_perm_simple_role_from_jwt passed\n");
}

void test_perm_simple_wildcard_permission(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"admin");
  csilk_perm_simple_allow("admin", "*", "*");

  int rc = csilk_perm_check(&c, "delete", "any_resource");
  assert(rc == 0);

  csilk_arena_free(c.arena);
  printf("test_perm_simple_wildcard_permission passed\n");
}

void test_perm_simple_wildcard_role(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"guest");
  csilk_perm_simple_allow("*", "read", "public:*");

  int rc = csilk_perm_check(&c, "read", "public:index.html");
  assert(rc == 0);

  csilk_arena_free(c.arena);
  printf("test_perm_simple_wildcard_role passed\n");
}

void test_perm_simple_deny_no_rule(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"viewer");

  int rc = csilk_perm_check(&c, "delete", "users:1");
  assert(rc == -1);

  csilk_arena_free(c.arena);
  printf("test_perm_simple_deny_no_rule passed\n");
}

void test_perm_simple_exact_match(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"admin");
  csilk_perm_simple_allow("admin", "delete", "server:config");

  int rc = csilk_perm_check(&c, "delete", "server:config");
  assert(rc == 0);

  rc = csilk_perm_check(&c, "delete", "server:other");
  assert(rc == -1);

  csilk_arena_free(c.arena);
  printf("test_perm_simple_exact_match passed\n");
}

void test_perm_simple_prefix_resource(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"editor");
  csilk_perm_simple_allow("editor", "write", "articles:*");

  int rc = csilk_perm_check(&c, "write", "articles:99");
  assert(rc == 0);

  rc = csilk_perm_check(&c, "write", "comments:1");
  assert(rc == -1);

  csilk_arena_free(c.arena);
  printf("test_perm_simple_prefix_resource passed\n");
}

void test_perm_custom_driver(void) {
  custom_check_called = 0;

  csilk_perm_register_driver("custom_test", &custom_driver);
  csilk_perm_set_default("custom_test");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));

  int rc = csilk_perm_check(&c, "any", "thing");
  assert(rc == 0);
  assert(custom_check_called == 1);

  printf("test_perm_custom_driver passed\n");
}

void test_perm_init_idempotent(void) {
  int count_before = 0;
  for (int i = 0;; i++) {
    const char* name;
    if (i == 0) name = "simple";
    else if (i == 1) name = "custom2";
    else if (i == 2) name = "custom_test";
    else break;
    if (csilk_perm_get_driver(name)) count_before++;
  }

  csilk_perm_init();

  int count_after = 0;
  for (int i = 0;; i++) {
    const char* name;
    if (i == 0) name = "simple";
    else if (i == 1) name = "custom2";
    else if (i == 2) name = "custom_test";
    else break;
    if (csilk_perm_get_driver(name)) count_after++;
  }

  assert(count_before == count_after);
  printf("test_perm_init_idempotent passed\n");
}

static void dummy_handler(csilk_ctx_t* c) {
  (void)c;
}

void test_perm_router_route_perm(void) {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_perm(r, "GET", "/admin/users", h, 1, "admin", "users:*");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.request.method = "GET";
  c.request.path = "/admin/users";

  int matched = csilk_router_match_ctx(r, &c);
  assert(matched);
  assert(c.current_handler != NULL);
  assert(c.current_handler->perm_required != NULL);
  assert(strcmp(c.current_handler->perm_required, "admin") == 0);
  assert(c.current_handler->perm_resource != NULL);
  assert(strcmp(c.current_handler->perm_resource, "users:*") == 0);

  csilk_router_free(r);
  printf("test_perm_router_route_perm passed\n");
}

void test_perm_router_route_extended_perm(void) {
  csilk_router_t* r = csilk_router_new();
  assert(r);

  csilk_handler_t h[] = {dummy_handler};
  csilk_router_add_extended_perm(r, "POST", "/articles", h, 1, "/articles",
                                 "CreateReq", "ArticleResp", "Create article",
                                 "Creates a new article", "write",
                                 "articles:*");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.request.method = "POST";
  c.request.path = "/articles";

  int matched = csilk_router_match_ctx(r, &c);
  assert(matched);
  assert(c.current_handler != NULL);
  assert(strcmp(c.current_handler->perm_required, "write") == 0);
  assert(strcmp(c.current_handler->perm_resource, "articles:*") == 0);
  assert(strcmp(c.current_handler->input_type, "CreateReq") == 0);
  assert(strcmp(c.current_handler->output_type, "ArticleResp") == 0);
  assert(strcmp(c.current_handler->summary, "Create article") == 0);
  assert(strcmp(c.current_handler->description, "Creates a new article") == 0);

  csilk_router_free(r);
  printf("test_perm_router_route_extended_perm passed\n");
}

void test_perm_auto_middleware_allowed(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"admin");
  csilk_perm_simple_allow("admin", "write", "articles:*");

  csilk_method_handler_t mh;
  memset(&mh, 0, sizeof(mh));
  mh.perm_required = "write";
  mh.perm_resource = "articles:42";
  c.current_handler = &mh;

  csilk_perm_auto_middleware(&c);
  assert(c.aborted == 0);

  csilk_arena_free(c.arena);
  printf("test_perm_auto_middleware_allowed passed\n");
}

void test_perm_auto_middleware_denied(void) {
  reset_simple();
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_set(&c, "role", (void*)"viewer");

  csilk_method_handler_t mh;
  memset(&mh, 0, sizeof(mh));
  mh.perm_required = "delete";
  mh.perm_resource = "users:1";
  c.current_handler = &mh;

  csilk_perm_auto_middleware(&c);
  assert(c.aborted != 0);

  csilk_arena_free(c.arena);
  printf("test_perm_auto_middleware_denied passed\n");
}

void test_perm_auto_middleware_no_perm(void) {
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));

  csilk_method_handler_t mh;
  memset(&mh, 0, sizeof(mh));
  mh.perm_required = NULL;
  c.current_handler = &mh;

  csilk_perm_auto_middleware(&c);
  assert(c.aborted == 0);

  printf("test_perm_auto_middleware_no_perm passed\n");
}

void test_perm_auto_middleware_no_handler(void) {
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.current_handler = NULL;

  csilk_perm_auto_middleware(&c);
  assert(c.aborted == 0);

  printf("test_perm_auto_middleware_no_handler passed\n");
}

int main(void) {
  csilk_perm_init();

  test_perm_init();
  test_perm_register_lookup();
  test_perm_set_default();
  test_perm_simple_no_role();
  test_perm_simple_role_from_csilk_get();
  test_perm_simple_role_from_jwt();
  test_perm_simple_wildcard_permission();
  test_perm_simple_wildcard_role();
  test_perm_simple_deny_no_rule();
  test_perm_simple_exact_match();
  test_perm_simple_prefix_resource();
  test_perm_custom_driver();
  test_perm_init_idempotent();
  test_perm_router_route_perm();
  test_perm_router_route_extended_perm();
  test_perm_auto_middleware_allowed();
  test_perm_auto_middleware_denied();
  test_perm_auto_middleware_no_perm();
  test_perm_auto_middleware_no_handler();

  printf("All perm tests passed\n");
  return 0;
}
