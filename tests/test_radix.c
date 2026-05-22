#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gin.h"

void handler_ping(gin_ctx_t* c) { (void)c; }
void handler_user(gin_ctx_t* c) { (void)c; }
void handler_static(gin_ctx_t* c) { (void)c; }

int main() {
  gin_router_t* r = gin_router_new();

  gin_handler_t h_ping[] = {handler_ping};
  gin_handler_t h_user[] = {handler_user};
  gin_handler_t h_static[] = {handler_static};

  gin_router_add(r, "GET", "/api/ping", h_ping, 1);
  gin_router_add(r, "GET", "/users/:id", h_user, 1);
  gin_router_add(r, "GET", "/static/*path", h_static, 1);

  // Test Static
  {
    gin_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = "/api/ping";
    int matched = gin_router_match_ctx(r, &ctx);
    assert(matched);
    assert(ctx.handlers[0] == handler_ping);
    assert(ctx.params_count == 0);
  }

  // Test Param
  {
    gin_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = "/users/123";
    int matched = gin_router_match_ctx(r, &ctx);
    assert(matched);
    assert(ctx.handlers[0] == handler_user);
    assert(ctx.params_count == 1);
    assert(strcmp(ctx.params[0].key, "id") == 0);
    assert(strcmp(ctx.params[0].value, "123") == 0);
    assert(strcmp(gin_get_param(&ctx, "id"), "123") == 0);

    gin_ctx_cleanup(&ctx);
  }

  // Test Wildcard
  {
    gin_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = "/static/js/app.js";
    int matched = gin_router_match_ctx(r, &ctx);
    assert(matched);
    assert(ctx.handlers[0] == handler_static);
    assert(ctx.params_count == 1);
    assert(strcmp(ctx.params[0].key, "path") == 0);
    assert(strcmp(ctx.params[0].value, "js/app.js") == 0);
    assert(strcmp(gin_get_param(&ctx, "path"), "js/app.js") == 0);

    gin_ctx_cleanup(&ctx);
  }

  // Test No Match
  {
    gin_ctx_t ctx = {0};
    ctx.request.method = "GET";
    ctx.request.path = "/unknown";
    int matched = gin_router_match_ctx(r, &ctx);
    assert(!matched);
  }

  gin_router_free(r);
  printf("test_radix: PASS\n");
  return 0;
}
