#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/protocols/h3.h"
#include "csilk/test/test.h"

static void
test_h3_varint()
{
    printf("Testing HTTP/3 RFC 9000 Varint encoding/decoding...\n");

    uint64_t test_vals[] = {0,
                            37,
                            63,
                            64,
                            15293,
                            16383,
                            16384,
                            4948729,
                            1073741823ULL,
                            1073741824ULL,
                            4611686018427387903ULL};
    size_t   count = sizeof(test_vals) / sizeof(test_vals[0]);

    for (size_t i = 0; i < count; i++) {
        uint8_t buf[16] = {0};
        size_t  enc_bytes = csilk_h3_varint_encode(test_vals[i], buf);
        assert(enc_bytes > 0);

        uint64_t decoded = 0;
        size_t   dec_bytes = csilk_h3_varint_decode(buf, sizeof(buf), &decoded);
        assert(dec_bytes == enc_bytes);
        assert(decoded == test_vals[i]);
    }

    printf("test_h3_varint: PASS\n");
}

static void
test_h3_frame_encode_decode()
{
    printf("Testing HTTP/3 Frame encoding/decoding...\n");

    const uint8_t payload[] = {'H', 'T', 'T', 'P', '/', '3', ' ', 'D', 'a', 't', 'a'};
    uint8_t       frame_buf[64] = {0};

    size_t frame_len = csilk_h3_frame_encode(
        CSILK_H3_FRAME_DATA, payload, sizeof(payload), frame_buf, sizeof(frame_buf));
    assert(frame_len > sizeof(payload));

    uint64_t decoded_type = 0ULL;
    size_t   payload_offset = 0;
    size_t   payload_len = 0;

    int res =
        csilk_h3_frame_decode(frame_buf, frame_len, &decoded_type, &payload_offset, &payload_len);
    assert(res == 0);
    assert(decoded_type == CSILK_H3_FRAME_DATA);
    assert(payload_len == sizeof(payload));
    assert(memcmp(frame_buf + payload_offset, payload, sizeof(payload)) == 0);

    printf("test_h3_frame_encode_decode: PASS\n");
}

static void
test_h3_alt_svc_header()
{
    printf("Testing HTTP/3 Alt-Svc header injection...\n");

    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_h3_inject_alt_svc_header(ctx, 443);
    const char* alt_svc = csilk_get_response_header(ctx, "Alt-Svc");
    assert(alt_svc != NULL);
    assert(strstr(alt_svc, "h3=\":443\"") != NULL);

    csilk_test_ctx_free(ctx);
    printf("test_h3_alt_svc_header: PASS\n");
}

int
main()
{
    test_h3_varint();
    test_h3_frame_encode_decode();
    test_h3_alt_svc_header();
    printf("All HTTP/3 protocol tests passed successfully!\n");
    return 0;
}
