#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

// Mock _csilk_send_response to capture the result
static int response_sent = 0;
void
_csilk_send_response(csilk_ctx_t* c)
{
    (void)c;
    response_sent = 1;
}

static void
mock_handler(csilk_ctx_t* c)
{
    char* body = malloc(2000);
    memset(body, 'A', 2000);
    body[1999] = '\0';
    csilk_set_response_body(c, body, 2000, 1);
    csilk_status(c, CSILK_STATUS_OK);
}

int
main()
{
    printf("Testing Gzip Middleware (Async)...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    char         mock_client_marker = 1;
    _csilk_set_internal_client(c, &mock_client_marker); // Mock internal client

    csilk_handler_t handlers[] = {csilk_gzip_middleware, mock_handler, nullptr};
    csilk_test_ctx_set_handlers(c, handlers);

    // Simulate request with Accept-Encoding: gzip
    csilk_set_request_header(c, "Accept-Encoding", "gzip");

    // Run middleware via next
    csilk_next(c);

    // Since Gzip has a hybrid sync/async path, only run the loop if async
    if (csilk_is_async(c)) {
        printf("Waiting for async gzip to complete...\n");
        csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    } else {
        printf("Sync gzip completed inline.\n");
        _csilk_send_response(c);
    }

    assert(response_sent == 1);

    const char* content_encoding = csilk_get_response_header(c, "Content-Encoding");
    assert(content_encoding != nullptr);
    assert(strcmp(content_encoding, "gzip") == 0);

    size_t      body_len = 0;
    const char* body = csilk_get_response_body(c, &body_len);
    assert(body_len < 2000);
    printf("Compressed size: %zu\n", body_len);

    // Verify it's actually valid gzip
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    assert(inflateInit2(&strm, 15 + 16) == Z_OK);

    char* decompressed = malloc(4096);
    strm.next_in = (Bytef*)body;
    strm.avail_in = (uInt)body_len;
    strm.next_out = (Bytef*)decompressed;
    strm.avail_out = 4096;

    int ret = inflate(&strm, Z_FINISH);
    assert(ret == Z_STREAM_END);
    assert(strm.total_out == 2000);
    for (int i = 0; i < 1999; i++) {
        assert(decompressed[i] == 'A');
    }

    inflateEnd(&strm);
    free(decompressed);

    csilk_test_ctx_free(c);

    printf("Gzip Middleware test passed!\n");
    return 0;
}
