#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static void test_csilk_next_aborted() {
  printf("Testing csilk_next with aborted context...\n");
  csilk_ctx_t ctx = {0};
  ctx.aborted = 1;
  ctx.handler_index = -1;
  csilk_handler_t handlers[] = {NULL};
  ctx.handlers = handlers;
  csilk_next(&ctx);
  assert(ctx.handler_index == -1);
  printf("csilk_next_aborted passed!\n");
}

static void test_csilk_next_null_handler() {
  printf("Testing csilk_next with NULL handler...\n");
  csilk_ctx_t ctx = {0};
  ctx.handler_index = -1;
  csilk_handler_t handlers[] = {NULL};
  ctx.handlers = handlers;
  csilk_next(&ctx);
  assert(ctx.handler_index == 0);
  printf("csilk_next_null_handler passed!\n");
}

static void test_csilk_abort() {
  printf("Testing csilk_abort...\n");
  csilk_ctx_t ctx = {0};
  assert(ctx.aborted == 0);
  csilk_abort(&ctx);
  assert(ctx.aborted == 1);
  printf("csilk_abort passed!\n");
}

static void test_csilk_status() {
  printf("Testing csilk_status...\n");
  csilk_ctx_t ctx = {0};
  csilk_status(&ctx, 404);
  assert(ctx.response.status == 404);
  printf("csilk_status passed!\n");
}

static void test_csilk_string_no_arena() {
  printf("Testing csilk_string with no arena...\n");
  csilk_ctx_t ctx = {0};
  csilk_string(&ctx, 200, "hello");
  assert(ctx.response.status == 200);
  assert(ctx.response.body != NULL);
  assert(strcmp(ctx.response.body, "hello") == 0);
  assert(ctx.response.body_is_managed == 1);
  if (ctx.response.body && ctx.response.body_is_managed) {
    free((void*)ctx.response.body);
    ctx.response.body = NULL;
  }
  printf("csilk_string_no_arena passed!\n");
}

static void test_csilk_string_with_arena() {
  printf("Testing csilk_string with arena...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_string(&ctx, 200, "arena hello");
  assert(ctx.response.status == 200);
  assert(ctx.response.body != NULL);
  assert(strcmp(ctx.response.body, "arena hello") == 0);
  assert(ctx.response.body_is_managed == 0);
  csilk_arena_free(ctx.arena);
  printf("csilk_string_with_arena passed!\n");
}

static void test_csilk_string_null_msg() {
  printf("Testing csilk_string with NULL message...\n");
  csilk_ctx_t ctx = {0};
  csilk_string(&ctx, 204, NULL);
  assert(ctx.response.status == 204);
  assert(ctx.response.body == NULL);
  assert(ctx.response.body_len == 0);
  printf("csilk_string_null_msg passed!\n");
}

static void test_csilk_get_param() {
  printf("Testing csilk_get_param...\n");
  csilk_ctx_t ctx = {0};
  ctx.params_count = 2;
  ctx.params[0].key = strdup("id");
  ctx.params[0].value = strdup("42");
  ctx.params[1].key = strdup("name");
  ctx.params[1].value = strdup("test");

  const char* v = csilk_get_param(&ctx, "id");
  assert(v != NULL && strcmp(v, "42") == 0);
  v = csilk_get_param(&ctx, "name");
  assert(v != NULL && strcmp(v, "test") == 0);
  v = csilk_get_param(&ctx, "missing");
  assert(v == NULL);

  free(ctx.params[0].key);
  free(ctx.params[0].value);
  free(ctx.params[1].key);
  free(ctx.params[1].value);
  printf("csilk_get_param passed!\n");
}

static void test_csilk_get_header() {
  printf("Testing csilk_get_header...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_set_request_header(&ctx, "X-Test", "value1");
  const char* v = csilk_get_header(&ctx, "X-Test");
  assert(v != NULL && strcmp(v, "value1") == 0);
  v = csilk_get_header(&ctx, "x-test");
  assert(v != NULL && strcmp(v, "value1") == 0);
  v = csilk_get_header(&ctx, "Missing");
  assert(v == NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_get_header passed!\n");
}

static void test_csilk_get_response_header() {
  printf("Testing csilk_get_response_header...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_set_header(&ctx, "X-Resp", "resp-val");
  const char* v = csilk_get_response_header(&ctx, "X-Resp");
  assert(v != NULL && strcmp(v, "resp-val") == 0);
  csilk_arena_free(ctx.arena);
  printf("csilk_get_response_header passed!\n");
}

static void test_csilk_get_query() {
  printf("Testing csilk_get_query...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_parse_query(&ctx, "foo=1&bar=baz");
  const char* v = csilk_get_query(&ctx, "foo");
  assert(v != NULL && strcmp(v, "1") == 0);
  v = csilk_get_query(&ctx, "bar");
  assert(v != NULL && strcmp(v, "baz") == 0);
  v = csilk_get_query(&ctx, "missing");
  assert(v == NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_get_query passed!\n");
}

static void test_csilk_get_method_path_body() {
  printf("Testing csilk_get_method/path/body...\n");
  csilk_ctx_t ctx = {0};
  ctx.request.method = "GET";
  ctx.request.path = strdup("/test");
  ctx.request.body = strdup("body data");
  ctx.request.body_len = 9;

  const char* m = csilk_get_method(&ctx);
  assert(m != NULL && strcmp(m, "GET") == 0);
  assert(csilk_get_method(NULL) == NULL);

  const char* p = csilk_get_path(&ctx);
  assert(p != NULL && strcmp(p, "/test") == 0);
  assert(csilk_get_path(NULL) == NULL);

  size_t blen = 0;
  const char* b = csilk_get_body(&ctx, &blen);
  assert(b != NULL && blen == 9 && strcmp(b, "body data") == 0);
  assert(csilk_get_body(NULL, NULL) == NULL);

  size_t blen2 = csilk_get_body_len(&ctx);
  assert(blen2 == 9);
  assert(csilk_get_body_len(NULL) == 0);

  free(ctx.request.path);
  free(ctx.request.body);
  printf("csilk_get_method/path/body passed!\n");
}

static void test_csilk_redirect() {
  printf("Testing csilk_redirect...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_redirect(&ctx, 301, "/new-location");
  assert(ctx.aborted == 1);
  assert(ctx.response.status == 301);
  const char* loc = csilk_get_response_header(&ctx, "Location");
  assert(loc != NULL && strcmp(loc, "/new-location") == 0);
  csilk_arena_free(ctx.arena);
  printf("csilk_redirect passed!\n");
}

static void test_csilk_redirect_invalid_status() {
  printf("Testing csilk_redirect with invalid status...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_redirect(&ctx, 200, "/fallback");
  assert(ctx.response.status == 302);
  csilk_arena_free(ctx.arena);
  printf("csilk_redirect_invalid_status passed!\n");
}

static void test_csilk_redirect_null() {
  printf("Testing csilk_redirect with NULL args...\n");
  csilk_ctx_t ctx = {0};
  csilk_redirect(NULL, 301, "/x");
  csilk_redirect(&ctx, 301, NULL);
  assert(ctx.aborted == 0);
  printf("csilk_redirect_null passed!\n");
}

static void test_csilk_redirect_simple() {
  printf("Testing csilk_redirect_simple...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_redirect_simple(&ctx, "/target");
  assert(ctx.response.status == 302);
  const char* loc = csilk_get_response_header(&ctx, "Location");
  assert(loc != NULL && strcmp(loc, "/target") == 0);
  csilk_arena_free(ctx.arena);
  printf("csilk_redirect_simple passed!\n");
}

static void test_csilk_bind_json_null() {
  printf("Testing csilk_bind_json with NULL input...\n");
  assert(csilk_bind_json(NULL) == NULL);
  csilk_ctx_t ctx = {0};
  assert(csilk_bind_json(&ctx) == NULL);
  printf("csilk_bind_json_null passed!\n");
}

static void test_csilk_bind_json_valid() {
  printf("Testing csilk_bind_json valid...\n");
  csilk_ctx_t ctx = {0};
  ctx.request.body = strdup("{\"key\":\"val\"}");
  cJSON* j = csilk_bind_json(&ctx);
  assert(j != NULL);
  cJSON* item = cJSON_GetObjectItem(j, "key");
  assert(item != NULL && cJSON_IsString(item));
  assert(strcmp(item->valuestring, "val") == 0);
  cJSON_Delete(j);
  free(ctx.request.body);
  printf("csilk_bind_json_valid passed!\n");
}

static void test_csilk_bind_json_err() {
  printf("Testing csilk_bind_json_err...\n");
  const char* err = NULL;
  assert(csilk_bind_json_err(NULL, &err) == NULL);
  assert(err != NULL);

  err = NULL;
  csilk_ctx_t ctx = {0};
  assert(csilk_bind_json_err(&ctx, &err) == NULL);
  assert(err != NULL);

  ctx.request.body = strdup("{invalid}");
  err = NULL;
  assert(csilk_bind_json_err(&ctx, &err) == NULL);
  assert(err != NULL);
  free(ctx.request.body);
  printf("csilk_bind_json_err passed!\n");
}

static void test_csilk_get_cookie_no_header() {
  printf("Testing csilk_get_cookie without Cookie header...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  const char* v = csilk_get_cookie(&ctx, "test");
  assert(v == NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_get_cookie_no_header passed!\n");
}

static void test_csilk_get_cookie_null() {
  printf("Testing csilk_get_cookie NULL args...\n");
  assert(csilk_get_cookie(NULL, "key") == NULL);
  csilk_ctx_t ctx = {0};
  assert(csilk_get_cookie(&ctx, NULL) == NULL);
  printf("csilk_get_cookie_null passed!\n");
}

static void test_csilk_get_cookie_with_header() {
  printf("Testing csilk_get_cookie with header...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_set_request_header(&ctx, "Cookie", "session=abc123; user=john");
  const char* v = csilk_get_cookie(&ctx, "session");
  assert(v != NULL && strcmp(v, "abc123") == 0);
  v = csilk_get_cookie(&ctx, "user");
  assert(v != NULL && strcmp(v, "john") == 0);
  v = csilk_get_cookie(&ctx, "missing");
  assert(v == NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_get_cookie_with_header passed!\n");
}

static void test_csilk_add_header() {
  printf("Testing csilk_add_header...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_add_header(&ctx, "Set-Cookie", "a=1");
  csilk_add_header(&ctx, "Set-Cookie", "b=2");
  csilk_add_header(&ctx, "Set-Cookie", "a=3");
  const char* v = csilk_get_response_header(&ctx, "Set-Cookie");
  assert(v != NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_add_header passed!\n");
}

static void test_csilk_set_cookie() {
  printf("Testing csilk_set_cookie...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_set_cookie(&ctx, "test", "value", 3600, "/app", "example.com", 1, 1);
  const char* cookie = csilk_get_response_header(&ctx, "Set-Cookie");
  assert(cookie != NULL);
  assert(strstr(cookie, "test=value") != NULL);
  assert(strstr(cookie, "Max-Age=3600") != NULL);
  assert(strstr(cookie, "Path=/app") != NULL);
  assert(strstr(cookie, "Domain=example.com") != NULL);
  assert(strstr(cookie, "Secure") != NULL);
  assert(strstr(cookie, "HttpOnly") != NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_set_cookie passed!\n");
}

static void test_csilk_set_cookie_negative_maxage() {
  printf("Testing csilk_set_cookie negative max_age...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_set_cookie(&ctx, "del", "", -1, NULL, NULL, 0, 0);
  const char* cookie = csilk_get_response_header(&ctx, "Set-Cookie");
  assert(cookie != NULL);
  assert(strstr(cookie, "Max-Age=0") != NULL);
  assert(strstr(cookie, "Path=/") != NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_set_cookie_negative_maxage passed!\n");
}

static void test_csilk_set_cookie_zero_maxage() {
  printf("Testing csilk_set_cookie zero max_age...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_set_cookie(&ctx, "sess", "val", 0, NULL, NULL, 0, 0);
  const char* cookie = csilk_get_response_header(&ctx, "Set-Cookie");
  assert(cookie != NULL);
  assert(strstr(cookie, "Max-Age") == NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_set_cookie_zero_maxage passed!\n");
}

static void test_csilk_set_cookie_no_arena() {
  printf("Testing csilk_set_cookie without arena...\n");
  csilk_ctx_t ctx = {0};
  csilk_set_cookie(&ctx, "x", "y", 0, NULL, NULL, 0, 0);
  assert(csilk_get_response_header(&ctx, "Set-Cookie") == NULL);
  printf("csilk_set_cookie_no_arena passed!\n");
}

static void test_csilk_json_null() {
  printf("Testing csilk_json with NULL...\n");
  csilk_json(NULL, 200, NULL);
  csilk_ctx_t ctx = {0};
  csilk_json(&ctx, 200, NULL);
  assert(ctx.response.body == NULL);
  printf("csilk_json_null passed!\n");
}

static void test_csilk_json_valid() {
  printf("Testing csilk_json valid...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  cJSON* j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "status", "ok");
  csilk_json(&ctx, 200, j);
  assert(ctx.response.status == 200);
  assert(ctx.response.body != NULL);
  assert(strstr(ctx.response.body, "status") != NULL);
  assert(ctx.response.body_is_managed == 1);
  const char* ct = csilk_get_response_header(&ctx, "Content-Type");
  assert(ct != NULL && strcmp(ct, "application/json") == 0);
  if (ctx.response.body && ctx.response.body_is_managed) {
    free((void*)ctx.response.body);
  }
  csilk_arena_free(ctx.arena);
  printf("csilk_json_valid passed!\n");
}

static void test_csilk_json_replaces_managed_body() {
  printf("Testing csilk_json replaces existing managed body...\n");
  csilk_ctx_t ctx = {0};
  ctx.response.body = strdup("old");
  ctx.response.body_is_managed = 1;
  ctx.response.body_len = 3;
  cJSON* j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "x", "y");
  csilk_json(&ctx, 200, j);
  assert(ctx.response.body != NULL);
  assert(strcmp(ctx.response.body, "old") != 0);
  if (ctx.response.body && ctx.response.body_is_managed) {
    free((void*)ctx.response.body);
  }
  printf("csilk_json_replaces_managed_body passed!\n");
}

static void test_csilk_json_error() {
  printf("Testing csilk_json_error...\n");
  csilk_json_error(NULL, 500, "err");
  csilk_ctx_t ctx = {0};
  csilk_json_error(&ctx, 400, "bad request");
  assert(ctx.response.status == 400);
  assert(ctx.response.body != NULL);
  assert(strstr(ctx.response.body, "bad request") != NULL);
  if (ctx.response.body && ctx.response.body_is_managed) {
    free((void*)ctx.response.body);
  }
  printf("csilk_json_error passed!\n");
}

static void test_csilk_json_error_null_msg() {
  printf("Testing csilk_json_error with NULL message...\n");
  csilk_ctx_t ctx = {0};
  csilk_json_error(&ctx, 500, NULL);
  assert(ctx.response.body != NULL);
  assert(strstr(ctx.response.body, "Unknown error") != NULL);
  if (ctx.response.body && ctx.response.body_is_managed) {
    free((void*)ctx.response.body);
  }
  printf("csilk_json_error_null_msg passed!\n");
}

static void test_csilk_get_status_is_websocket_is_sse_is_async() {
  printf("Testing csilk_get_status/is_websocket/is_sse/is_async...\n");
  csilk_ctx_t ctx = {0};
  assert(csilk_get_status(&ctx) == 0);
  assert(csilk_get_status(NULL) == 0);
  ctx.response.status = 200;
  assert(csilk_get_status(&ctx) == 200);

  assert(csilk_is_websocket(&ctx) == 0);
  ctx.is_websocket = 1;
  assert(csilk_is_websocket(&ctx) == 1);
  assert(csilk_is_websocket(NULL) == 0);

  assert(csilk_is_sse(&ctx) == 0);
  ctx.is_sse = 1;
  assert(csilk_is_sse(&ctx) == 1);
  assert(csilk_is_sse(NULL) == 0);

  assert(csilk_is_async(&ctx) == 0);
  csilk_set_async(&ctx, 1);
  assert(csilk_is_async(&ctx) == 1);
  csilk_set_async(NULL, 1);
  assert(csilk_is_async(NULL) == 0);
  printf("csilk_get_status/is_websocket/is_sse/is_async passed!\n");
}

static void test_csilk_get_response_body() {
  printf("Testing csilk_get_response_body...\n");
  assert(csilk_get_response_body(NULL, NULL) == NULL);
  size_t len = 99;
  assert(csilk_get_response_body(NULL, &len) == NULL);
  assert(len == 0);

  csilk_ctx_t ctx = {0};
  ctx.response.body = "resp";
  ctx.response.body_len = 4;
  len = 0;
  const char* b = csilk_get_response_body(&ctx, &len);
  assert(b != NULL && len == 4 && strcmp(b, "resp") == 0);
  printf("csilk_get_response_body passed!\n");
}

static void test_csilk_set_response_body() {
  printf("Testing csilk_set_response_body...\n");
  csilk_set_response_body(NULL, NULL, 0, 0);
  csilk_ctx_t ctx = {0};
  csilk_set_response_body(&ctx, "external", 8, 0);
  assert(ctx.response.body != NULL);
  assert(ctx.response.body_is_managed == 0);
  assert(ctx.response.body_len == 8);
  printf("csilk_set_response_body passed!\n");
}

static void test_csilk_set_response_body_replaces_managed() {
  printf("Testing csilk_set_response_body replaces managed...\n");
  csilk_ctx_t ctx = {0};
  ctx.response.body = strdup("old_managed");
  ctx.response.body_is_managed = 1;
  ctx.response.body_len = 11;
  csilk_set_response_body(&ctx, "new", 3, 0);
  assert(strcmp(ctx.response.body, "new") == 0);
  assert(ctx.response.body_is_managed == 0);
  printf("csilk_set_response_body_replaces_managed passed!\n");
}

static void test_csilk_is_aborted() {
  printf("Testing csilk_is_aborted...\n");
  assert(csilk_is_aborted(NULL) == 0);
  csilk_ctx_t ctx = {0};
  assert(csilk_is_aborted(&ctx) == 0);
  ctx.aborted = 1;
  assert(csilk_is_aborted(&ctx) == 1);
  printf("csilk_is_aborted passed!\n");
}

static void test_csilk_get_request_id() {
  printf("Testing csilk_get_request_id...\n");
  assert(csilk_get_request_id(NULL) == NULL);
  csilk_ctx_t ctx = {0};
  const char* id = csilk_get_request_id(&ctx);
  assert(id != NULL);
  assert(id[0] == '\0');
  strcpy(ctx.request_id, "abc-123");
  id = csilk_get_request_id(&ctx);
  assert(id != NULL && strcmp(id, "abc-123") == 0);
  printf("csilk_get_request_id passed!\n");
}

static void test_csilk_set_on_ws_message() {
  printf("Testing csilk_set_on_ws_message...\n");
  csilk_ctx_t ctx = {0};
  csilk_set_on_ws_message(NULL, NULL);
  assert(ctx.on_ws_message == NULL);
  csilk_set_on_ws_message(
      &ctx, (void (*)(csilk_ctx_t*, const uint8_t*, size_t, int))0x1);
  assert(ctx.on_ws_message != NULL);
  csilk_set_on_ws_message(&ctx, NULL);
  assert(ctx.on_ws_message == NULL);
  printf("csilk_set_on_ws_message passed!\n");
}

static void test_csilk_parse_query() {
  printf("Testing csilk_parse_query...\n");
  csilk_ctx_t ctx = {0};
  csilk_parse_query(&ctx, NULL);
  csilk_parse_query(&ctx, "");

  ctx.arena = csilk_arena_new(1024);
  csilk_parse_query(&ctx, "a=1&b=&c&d=%48%65%6C%6C%6F");
  const char* v = csilk_get_query(&ctx, "a");
  assert(v != NULL && strcmp(v, "1") == 0);
  v = csilk_get_query(&ctx, "b");
  assert(v != NULL && strcmp(v, "") == 0);
  v = csilk_get_query(&ctx, "c");
  assert(v != NULL && strcmp(v, "") == 0);
  v = csilk_get_query(&ctx, "d");
  assert(v != NULL && strcmp(v, "Hello") == 0);
  v = csilk_get_query(&ctx, "missing");
  assert(v == NULL);
  csilk_arena_free(ctx.arena);
  printf("csilk_parse_query passed!\n");
}

static void test_csilk_ctx_cleanup_null() {
  printf("Testing csilk_ctx_cleanup with NULL...\n");
  csilk_ctx_cleanup(NULL);
  printf("csilk_ctx_cleanup_null passed!\n");
}

static void test_csilk_ctx_cleanup_basic() {
  printf("Testing csilk_ctx_cleanup basic...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  ctx.request.body = strdup("body");
  ctx.request.path = strdup("/path");
  ctx.params_count = 1;
  ctx.params[0].key = strdup("k");
  ctx.params[0].value = strdup("v");
  ctx.file_fd = -1;
  csilk_set_request_header(&ctx, "X-Req", "h");
  csilk_parse_query(&ctx, "q=1");

  csilk_ctx_cleanup(&ctx);
  assert(ctx.request.body == NULL);
  assert(ctx.request.path == NULL);
  assert(ctx.params_count == 0);
  assert(ctx.aborted == 0);
  assert(ctx.is_websocket == 0);
  assert(ctx.is_sse == 0);
  assert(ctx.file_fd == -1);
  csilk_arena_free(ctx.arena);
  printf("csilk_ctx_cleanup_basic passed!\n");
}

static void test_csilk_set_get_storage() {
  printf("Testing csilk_set/csilk_get storage...\n");
  int val1 = 1, val2 = 2;
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);

  assert(csilk_get(&ctx, "nonexistent") == NULL);
  csilk_set(&ctx, "key1", &val1);
  csilk_set(&ctx, "key2", &val2);
  void* v = csilk_get(&ctx, "key1");
  assert(v == &val1);
  v = csilk_get(&ctx, "key2");
  assert(v == &val2);

  csilk_set(&ctx, "key1", &val2);
  v = csilk_get(&ctx, "key1");
  assert(v == &val2);

  csilk_set(&ctx, "key1", NULL);
  v = csilk_get(&ctx, "key1");
  assert(v == NULL);

  csilk_arena_free(ctx.arena);
  printf("csilk_set_get_storage passed!\n");
}

static void test_csilk_set_get_null() {
  printf("Testing csilk_set/csilk_get NULL...\n");
  csilk_set(NULL, "k", NULL);
  csilk_set(NULL, NULL, NULL);
  csilk_ctx_t ctx = {0};
  csilk_set(&ctx, NULL, NULL);
  assert(csilk_get(NULL, "k") == NULL);
  assert(csilk_get(&ctx, NULL) == NULL);
  printf("csilk_set_get_null passed!\n");
}

static void test_csilk_set_no_arena() {
  printf("Testing csilk_set without arena...\n");
  csilk_ctx_t ctx = {0};
  int x = 42;
  csilk_set(&ctx, "key", &x);
  assert(csilk_get(&ctx, "key") == NULL);
  printf("csilk_set_no_arena passed!\n");
}

static char driver_val[32] = {0};

static void test_driver_set(csilk_ctx_t* c, const char* key, void* value) {
  if (value)
    snprintf(driver_val, sizeof(driver_val), "%s:%s", key, (char*)value);
  else
    driver_val[0] = '\0';
}

static void* test_driver_get(csilk_ctx_t* c, const char* key) {
  return driver_val[0] ? driver_val : NULL;
}

static void test_driver_clear(csilk_ctx_t* c) { driver_val[0] = '\0'; }

static void test_csilk_set_get_storage_driver() {
  printf("Testing csilk_set/csilk_get with storage driver...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);

  csilk_storage_driver_t driver;
  memset(&driver, 0, sizeof(driver));
  driver.set = test_driver_set;
  driver.get = test_driver_get;
  driver.clear = test_driver_clear;

  ctx.storage_driver = &driver;
  csilk_set(&ctx, "name", (void*)"john");
  void* v = csilk_get(&ctx, "name");
  assert(v != NULL && strcmp((char*)v, "name:john") == 0);

  csilk_arena_free(ctx.arena);
  printf("csilk_set_get_storage_driver passed!\n");
}

static void test_csilk_bind_reflect() {
  printf("Testing csilk_bind_reflect...\n");
  assert(csilk_bind_reflect(NULL, NULL, NULL) == 0);
  csilk_ctx_t ctx = {0};
  assert(csilk_bind_reflect(&ctx, NULL, NULL) == 0);
  ctx.request.body = strdup("{}");
  int dummy;
  assert(csilk_bind_reflect(&ctx, "nonexistent", &dummy) == 0);
  free(ctx.request.body);
  printf("csilk_bind_reflect passed!\n");
}

static void test_csilk_json_reflect() {
  printf("Testing csilk_json_reflect...\n");
  csilk_json_reflect(NULL, 200, NULL, NULL);
  csilk_ctx_t ctx = {0};
  int dummy = 42;
  csilk_json_reflect(&ctx, 200, NULL, &dummy);
  assert(ctx.response.body == NULL);
  assert(ctx.response.status == 0);
  csilk_json_reflect(&ctx, 200, "nonexistent", &dummy);
  printf("csilk_json_reflect passed!\n");
}

static void test_csilk_response_write_end_null() {
  printf("Testing csilk_response_write/end with NULL...\n");
  csilk_response_write(NULL, NULL, 0);
  csilk_response_end(NULL);
  printf("csilk_response_write/end_null passed!\n");
}

static void test_csilk_parse_form_urlencoded_null() {
  printf("Testing csilk_parse_form_urlencoded NULL...\n");
  csilk_parse_form_urlencoded(NULL);
  csilk_ctx_t ctx = {0};
  csilk_parse_form_urlencoded(&ctx);
  printf("csilk_parse_form_urlencoded_null passed!\n");
}

static void test_csilk_get_form_field_null() {
  printf("Testing csilk_get_form_field NULL...\n");
  assert(csilk_get_form_field(NULL, "k") == NULL);
  csilk_ctx_t ctx = {0};
  assert(csilk_get_form_field(&ctx, NULL) == NULL);
  printf("csilk_get_form_field_null passed!\n");
}

int main() {
  test_csilk_next_aborted();
  test_csilk_next_null_handler();
  test_csilk_abort();
  test_csilk_status();
  test_csilk_string_no_arena();
  test_csilk_string_with_arena();
  test_csilk_string_null_msg();
  test_csilk_get_param();
  test_csilk_get_header();
  test_csilk_get_response_header();
  test_csilk_get_query();
  test_csilk_get_method_path_body();
  test_csilk_redirect();
  test_csilk_redirect_invalid_status();
  test_csilk_redirect_null();
  test_csilk_redirect_simple();
  test_csilk_bind_json_null();
  test_csilk_bind_json_valid();
  test_csilk_bind_json_err();
  test_csilk_get_cookie_no_header();
  test_csilk_get_cookie_null();
  test_csilk_get_cookie_with_header();
  test_csilk_add_header();
  test_csilk_set_cookie();
  test_csilk_set_cookie_negative_maxage();
  test_csilk_set_cookie_zero_maxage();
  test_csilk_set_cookie_no_arena();
  test_csilk_json_null();
  test_csilk_json_valid();
  test_csilk_json_replaces_managed_body();
  test_csilk_json_error();
  test_csilk_json_error_null_msg();
  test_csilk_get_status_is_websocket_is_sse_is_async();
  test_csilk_get_response_body();
  test_csilk_set_response_body();
  test_csilk_set_response_body_replaces_managed();
  test_csilk_is_aborted();
  test_csilk_get_request_id();
  test_csilk_set_on_ws_message();
  test_csilk_parse_query();
  test_csilk_ctx_cleanup_null();
  test_csilk_ctx_cleanup_basic();
  test_csilk_set_get_storage();
  test_csilk_set_get_null();
  test_csilk_set_no_arena();
  test_csilk_set_get_storage_driver();
  test_csilk_bind_reflect();
  test_csilk_json_reflect();
  test_csilk_response_write_end_null();
  test_csilk_parse_form_urlencoded_null();
  test_csilk_get_form_field_null();
  printf("test_context_ext: ALL PASSED\n");
  return 0;
}
