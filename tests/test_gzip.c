#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "csilk.h"
#include "csilk_internal.h"

// Mock _csilk_send_response to capture the result
static int response_sent = 0;
void _csilk_send_response(csilk_ctx_t* c) { response_sent = 1; }

static void mock_handler(csilk_ctx_t* c) {
  char* body = malloc(2000);
  memset(body, 'A', 2000);
  body[1999] = '\0';
  c->response.body = body;
  c->response.body_len = 2000;
  c->response.body_is_managed = 1;
  c->response.status = CSILK_STATUS_OK;
}

int main() {
  printf("Testing Gzip Middleware (Async)...\n");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(4096);
  char mock_client_marker = 1;
  c._internal_client = &mock_client_marker;  // Mock internal client

  csilk_handler_t handlers[] = {csilk_gzip_middleware, mock_handler, NULL};
  c.handlers = handlers;
  c.handler_index = -1;

  // Simulate request with Accept-Encoding: gzip
  csilk_set_request_header(&c, "Accept-Encoding", "gzip");

  // Run middleware via next
  csilk_next(&c);

  // Since it's async, we need to run the loop
  assert(c.is_async == 1);

  printf("Waiting for async gzip to complete...\n");
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  assert(response_sent == 1);

  const char* content_encoding = NULL;
  for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
    csilk_header_t* h = c.response.headers.buckets[i];
    while (h) {
      if (strcasecmp(h->key, "Content-Encoding") == 0) {
        content_encoding = h->value;
        break;
      }
      h = h->next;
    }
    if (content_encoding) break;
  }
  assert(content_encoding != NULL);
  assert(strcmp(content_encoding, "gzip") == 0);

  assert(c.response.body_len < 2000);
  printf("Compressed size: %zu\n", c.response.body_len);

  // Verify it's actually valid gzip
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  assert(inflateInit2(&strm, 15 + 16) == Z_OK);

  char* decompressed = malloc(4096);
  strm.next_in = (Bytef*)c.response.body;
  strm.avail_in = (uInt)c.response.body_len;
  strm.next_out = (Bytef*)decompressed;
  strm.avail_out = 4096;

  int ret = inflate(&strm, Z_FINISH);
  assert(ret == Z_STREAM_END);
  assert(strm.total_out == 2000);
  for (int i = 0; i < 1999; i++) assert(decompressed[i] == 'A');

  inflateEnd(&strm);
  free(decompressed);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);

  printf("Gzip Middleware test passed!\n");
  return 0;
}
