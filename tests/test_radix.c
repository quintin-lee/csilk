#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

void handler_ping(csilk_ctx_t* c) { (void)c; }
void handler_user(csilk_ctx_t* c) { (void)c; }
void handler_static(csilk_ctx_t* c) { (void)c; }

int main() {
  csilk_router_t* r = csilk_router_new();

  csilk_handler_t h_ping[] = {handler_ping};
  csilk_handler_t h_user[] = {handler_user};
  csilk_handler_t h_static[] = {handler_static};

  csilk_router_add(r, "GET", "/api/ping", h_ping, 1);
  csilk_router_add(r, "GET", "/users/:id", h_user, 1);
  csilk_router_add(r, "GET", "/static/*path", h_static, 1);

  // Test Static
  {
    csilk_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = strdup("/api/ping");
    int matched = csilk_router_match_ctx(r, &ctx);
    assert(matched);
    assert(ctx.handlers[0] == handler_ping);
    assert(ctx.params_count == 0);
    csilk_ctx_cleanup(&ctx);
  }

  // Test Param
  {
    csilk_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = strdup("/users/123");
    int matched = csilk_router_match_ctx(r, &ctx);
    assert(matched);
    assert(ctx.handlers[0] == handler_user);
    assert(ctx.params_count == 1);
    assert(strcmp(ctx.params[0].key, "id") == 0);
    assert(strcmp(ctx.params[0].value, "123") == 0);
    assert(strcmp(csilk_get_param(&ctx, "id"), "123") == 0);

    csilk_ctx_cleanup(&ctx);
  }

  // Test Wildcard
  {
    csilk_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = strdup("/static/js/app.js");
    int matched = csilk_router_match_ctx(r, &ctx);
    assert(matched);
    assert(ctx.handlers[0] == handler_static);
    assert(ctx.params_count == 1);
    assert(strcmp(ctx.params[0].key, "path") == 0);
    assert(strcmp(ctx.params[0].value, "js/app.js") == 0);
    assert(strcmp(csilk_get_param(&ctx, "path"), "js/app.js") == 0);

    csilk_ctx_cleanup(&ctx);
  }

  // Test No Match
  {
    csilk_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = strdup("/unknown");
    int matched = csilk_router_match_ctx(r, &ctx);
    assert(!matched);
    csilk_ctx_cleanup(&ctx);
  }

  // Test Boundary / NULL
  {
    int matched;
    matched = csilk_router_match_ctx(NULL, NULL);
    assert(!matched);

    csilk_ctx_t ctx = {0};
    matched = csilk_router_match_ctx(r, &ctx);
    assert(!matched);  // missing method and path

    ctx.request.method = "GET";
    matched = csilk_router_match_ctx(r, &ctx);
    assert(!matched);  // missing path
  }

  csilk_router_free(r);
  printf("test_radix: PASS\n");
  return 0;
}
