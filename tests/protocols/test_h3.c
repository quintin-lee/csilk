#include <assert.h>
#include <errno.h>
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
test_h3_varint_edge_cases(void)
{
    printf("Testing HTTP/3 Varint edge cases and truncation...\n");

    /* Null buffer */
    assert(csilk_h3_varint_encode(10, nullptr) == 0);
    assert(csilk_h3_varint_decode(nullptr, 10, nullptr) == 0);

    /* Value exceeds 62 bits */
    uint8_t  buf[16] = {0};
    uint64_t huge_val = (1ULL << 62);
    assert(csilk_h3_varint_encode(huge_val, buf) == 0);

    /* Zero length decode */
    uint64_t decoded = 0;
    assert(csilk_h3_varint_decode(buf, 0, &decoded) == 0);

    /* Truncated multi-byte varints */
    /* 2-byte varint needs 2 bytes, provide only 1 */
    buf[0] = 0x40 | 0x01;
    assert(csilk_h3_varint_decode(buf, 1, &decoded) == 0);

    /* 4-byte varint needs 4 bytes, provide only 2 */
    buf[0] = 0x80 | 0x01;
    assert(csilk_h3_varint_decode(buf, 2, &decoded) == 0);

    /* 8-byte varint needs 8 bytes, provide only 4 */
    buf[0] = 0xC0 | 0x01;
    assert(csilk_h3_varint_decode(buf, 4, &decoded) == 0);

    printf("test_h3_varint_edge_cases: PASS\n");
}

static void
test_h3_frame_edge_cases(void)
{
    printf("Testing HTTP/3 Frame buffer limits and malformed decodes...\n");

    uint8_t out[32];
    /* Buffer too small */
    assert(csilk_h3_frame_encode(CSILK_H3_FRAME_DATA, (const uint8_t*)"1234567890", 10, out, 5) ==
           0);
    assert(csilk_h3_frame_encode(CSILK_H3_FRAME_DATA, nullptr, 0, nullptr, 100) == 0);

    /* Frame decode with null parameters */
    uint64_t type = 0;
    size_t   offset = 0;
    size_t   len = 0;
    assert(csilk_h3_frame_decode(nullptr, 10, &type, &offset, &len) == -1);
    assert(csilk_h3_frame_decode(out, 10, nullptr, &offset, &len) == -1);
    assert(csilk_h3_frame_decode(out, 10, &type, nullptr, &len) == -1);
    assert(csilk_h3_frame_decode(out, 10, &type, &offset, nullptr) == -1);

    /* Incomplete / truncated payload */
    size_t valid_len = csilk_h3_frame_encode(
        CSILK_H3_FRAME_HEADERS, (const uint8_t*)"header", 6, out, sizeof(out));
    assert(valid_len > 0);
    /* Cut off 2 bytes from payload */
    assert(csilk_h3_frame_decode(out, valid_len - 2, &type, &offset, &len) == -1);

    /* Empty frame decode */
    assert(csilk_h3_frame_decode(out, 0, &type, &offset, &len) == -1);

    printf("test_h3_frame_edge_cases: PASS\n");
}

static void
test_h3_alt_svc_and_listener_edge_cases(void)
{
    printf("Testing Alt-Svc invalid args and listener edge cases...\n");

    /* Alt-Svc null context and invalid ports */
    csilk_h3_inject_alt_svc_header(nullptr, 443);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_h3_inject_alt_svc_header(ctx, 0);
    const char* alt_svc = csilk_get_response_header(ctx, "Alt-Svc");
    assert(alt_svc != nullptr && strstr(alt_svc, "h3=\":443\"") != nullptr);
    csilk_test_ctx_free(ctx);

    /* Listener invalid binds */
    assert(csilk_h3_listener_bind(0) == nullptr);
    assert(csilk_h3_listener_bind(-1) == nullptr);
    assert(csilk_h3_listener_bind(80000) == nullptr);

    /* Listener null close */
    csilk_h3_listener_close(nullptr);

    /* Listener process packet invalid args */
    uint64_t conn_id = 0;
    uint8_t  pkt[16] = {0xC0, 1, 2, 3, 4, 5, 6, 7, 8};
    assert(csilk_h3_listener_process_packet(nullptr, pkt, 16, &conn_id) == -1);

#if CSILK_ENABLE_EXPERIMENTAL_STUBS
    csilk_h3_listener_t* l = csilk_h3_listener_bind(9443);
    assert(l != nullptr);
    assert(csilk_h3_listener_process_packet(l, nullptr, 16, &conn_id) == -1);
    assert(csilk_h3_listener_process_packet(l, pkt, 5, &conn_id) == -1); /* too short */
    assert(csilk_h3_listener_process_packet(l, pkt, 16, nullptr) == -1);
    csilk_h3_listener_close(l);
#endif

    printf("test_h3_alt_svc_and_listener_edge_cases: PASS\n");
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

static void
test_h3_listener()
{
    printf("Testing HTTP/3 UDP Socket Listener & Connection ID extraction...\n");

    csilk_h3_listener_t* listener = csilk_h3_listener_bind(4433);
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    assert(listener == NULL);
    assert(errno == ENOTSUP);
    printf("test_h3_listener (fail-closed): PASS\n");
    return;
#else
    assert(listener != NULL);

    /* Construct mock QUIC packet header with 64-bit Connection ID: 0x1122334455667788 */
    uint8_t  pkt[12] = {0xC0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x00, 0x00};
    uint64_t conn_id = 0;

    int res = csilk_h3_listener_process_packet(listener, pkt, sizeof(pkt), &conn_id);
    assert(res == 0);
    assert(conn_id == 0x1122334455667788ULL);

    csilk_h3_listener_close(listener);
    printf("test_h3_listener: PASS\n");
#endif
}

int
main()
{
    test_h3_varint();
    test_h3_varint_edge_cases();
    test_h3_frame_encode_decode();
    test_h3_frame_edge_cases();
    test_h3_alt_svc_header();
    test_h3_alt_svc_and_listener_edge_cases();
    test_h3_listener();
    printf("All HTTP/3 protocol tests passed successfully!\n");
    return 0;
}
