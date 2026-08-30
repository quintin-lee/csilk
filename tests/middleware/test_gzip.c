#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

/* Mock _csilk_send_response to capture the result. */
static int response_sent = 0;
void
_csilk_send_response(csilk_ctx_t* c)
{
    (void)c;
    response_sent = 1;
}

/* Configurable mock downstream handler body. */
static size_t g_mock_body_len = 2000;

static void
mock_handler(csilk_ctx_t* c)
{
    char* body = malloc(g_mock_body_len ? g_mock_body_len : 1);
    for (size_t i = 0; i < g_mock_body_len; i++) {
        body[i] = 'A';
    }
    csilk_set_response_body(c, body, g_mock_body_len, 1);
    csilk_status(c, CSILK_STATUS_OK);
}

/* Run the gzip middleware chain on a fresh ctx with the given request setup.
 * Returns the ctx (caller must free). If expect_async, drives the event loop
 * so the thread-pool offload and its after-work callback complete. */
static csilk_ctx_t*
run_gzip(int         expect_async,
         size_t      body_len,
         const char* accept_encoding,
         const char* set_encoding,
         const char* content_type)
{
    g_mock_body_len = body_len;
    csilk_ctx_t* c = csilk_test_ctx_new();
    static char  mock_client_marker = 1;
    _csilk_set_internal_client(c, &mock_client_marker);

    csilk_handler_t handlers[] = {csilk_gzip_middleware, mock_handler, nullptr};
    csilk_test_ctx_set_handlers(c, handlers);

    if (accept_encoding) {
        csilk_set_request_header(c, "Accept-Encoding", accept_encoding);
    }
    if (set_encoding) {
        csilk_set_request_header(c, "Content-Encoding", set_encoding);
    }
    if (content_type) {
        csilk_set_request_header(c, "Content-Type", content_type);
    }

    response_sent = 0;
    csilk_next(c);

    if (expect_async) {
        assert(csilk_is_async(c) == 1);
        csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    } else {
        /* Sync path: the response is sent by the dispatch machinery, so the
         * test drives it manually (mirrors the original test). */
        assert(csilk_is_async(c) == 0);
        _csilk_send_response(c);
    }
    return c;
}

/* Verify the body is a valid gzip stream that decompresses to the expected
 * repeated-fill payload of `orig_len` bytes. */
static void
assert_valid_gzip(csilk_ctx_t* c, size_t orig_len)
{
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(c, &body_len);

    const char* content_encoding = csilk_get_response_header(c, "Content-Encoding");
    assert(content_encoding != nullptr);
    assert(strcmp(content_encoding, "gzip") == 0);
    assert(body_len > 0);
    assert(body_len < orig_len);

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    assert(inflateInit2(&strm, 15 + 16) == Z_OK);
    char* decompressed = malloc(orig_len + 1);
    assert(decompressed != nullptr);
    strm.next_in = (Bytef*)body;
    strm.avail_in = (uInt)body_len;
    strm.next_out = (Bytef*)decompressed;
    strm.avail_out = (uInt)orig_len + 1;
    int ret = inflate(&strm, Z_FINISH);
    assert(ret == Z_STREAM_END);
    assert(strm.total_out == orig_len);
    for (size_t i = 0; i < orig_len; i++) {
        assert(decompressed[i] == 'A');
    }
    inflateEnd(&strm);
    free(decompressed);
}

static void
test_gzip_sync_happy(void)
{
    printf("Testing gzip sync happy path...\n");
    csilk_ctx_t* c = run_gzip(0, 2000, "gzip", NULL, NULL);
    assert(response_sent == 1);
    assert_valid_gzip(c, 2000);
    csilk_test_ctx_free(c);
    printf("sync happy path passed!\n");
}

static void
test_gzip_async_offload(void)
{
    printf("Testing gzip async thread-pool offload (>32KB body)...\n");
    csilk_ctx_t* c = run_gzip(1, 50000, "gzip", NULL, NULL);
    assert(response_sent == 1);
    assert_valid_gzip(c, 50000);
    csilk_test_ctx_free(c);
    printf("async offload passed!\n");
}

static void
test_gzip_skip_empty_body(void)
{
    printf("Testing gzip skip empty body...\n");
    csilk_ctx_t* c = run_gzip(0, 0, "gzip", NULL, NULL);
    size_t       body_len;
    const char*  body = csilk_get_response_body(c, &body_len);
    assert(body_len == 0 || body == NULL);
    assert(csilk_get_response_header(c, "Content-Encoding") == NULL);
    csilk_test_ctx_free(c);
    printf("skip empty body passed!\n");
}

static void
test_gzip_skip_already_encoded(void)
{
    printf("Testing gzip skip already-encoded...\n");
    csilk_ctx_t* c = run_gzip(0, 2000, "gzip", "br", NULL);
    /* The middleware sees a request Content-Encoding of "br" and skips, so
     * the response body is untouched (still the original 2000 A's) and no
     * gzip response encoding is applied. */
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(c, &body_len);
    assert(body_len == 2000);
    assert(body != NULL);
    assert(csilk_get_response_header(c, "Content-Encoding") == NULL);
    csilk_test_ctx_free(c);
    printf("skip already-encoded passed!\n");
}

static void
test_gzip_skip_incompressible(void)
{
    printf("Testing gzip skip incompressible content-type...\n");
    csilk_ctx_t* c = run_gzip(0, 2000, "gzip", NULL, "image/png");
    assert(csilk_get_response_header(c, "Content-Encoding") == NULL);
    csilk_test_ctx_free(c);
    printf("skip incompressible passed!\n");
}

static void
test_gzip_skip_no_accept_encoding(void)
{
    printf("Testing gzip skip missing Accept-Encoding gzip...\n");
    csilk_ctx_t* c = run_gzip(0, 2000, "deflate", NULL, NULL);
    assert(csilk_get_response_header(c, "Content-Encoding") == NULL);
    csilk_test_ctx_free(c);
    printf("skip no accept-encoding passed!\n");
}

static void
test_gzip_skip_below_min_length(void)
{
    printf("Testing gzip skip body below minimum length...\n");
    csilk_ctx_t* c = run_gzip(0, 100, "gzip", NULL, NULL);
    assert(csilk_get_response_header(c, "Content-Encoding") == NULL);
    csilk_test_ctx_free(c);
    printf("skip below min length passed!\n");
}

int
main(void)
{
    setbuf(stdout, NULL);
    printf("Testing Gzip Middleware...\n");
    test_gzip_sync_happy();
    test_gzip_async_offload();
    test_gzip_skip_empty_body();
    test_gzip_skip_already_encoded();
    test_gzip_skip_incompressible();
    test_gzip_skip_no_accept_encoding();
    test_gzip_skip_below_min_length();
    printf("test_gzip: ALL PASSED\n");
    return 0;
}