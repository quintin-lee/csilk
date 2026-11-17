/*
 * tests/protocols/test_grpc.c
 *
 * Unit tests for gRPC frame parsing, encoding, and trailer handling.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/protocols/grpc.h"

static void
test_frame_encode(void)
{
    printf("Testing gRPC frame encode...\n");

    uint8_t payload[] = {0x0A, 0x01, 0x02};
    uint8_t buf[64];

    int len = csilk_grpc_frame_encode_ex(payload, 3, 0, buf, sizeof(buf));
    assert(len == 8); /* 5 header + 3 payload */

    /* Verify header */
    assert(buf[0] == 0); /* uncompressed */
    assert(buf[1] == 0);
    assert(buf[2] == 0);
    assert(buf[3] == 0);
    assert(buf[4] == 3);

    /* Verify payload */
    assert(memcmp(buf + 5, payload, 3) == 0);

    /* Compressed frame */
    len = csilk_grpc_frame_encode_ex(payload, 3, 1, buf, sizeof(buf));
    assert(len == 8);
    assert(buf[0] == 1); /* compressed */

    /* Too small buffer */
    len = csilk_grpc_frame_encode_ex(payload, 3, 0, buf, 7);
    assert(len == -1);

    /* Null payload with len 0 */
    len = csilk_grpc_frame_encode_ex(NULL, 0, 0, buf, sizeof(buf));
    assert(len == 5); /* just the header */

    printf("test_frame_encode: PASS\n");
}

static void
test_frame_parse(void)
{
    printf("Testing gRPC frame parse...\n");

    uint8_t        buf[] = {0x00, 0x00, 0x00, 0x00, 0x04, 0xAA, 0xBB, 0xCC, 0xDD};
    const uint8_t* payload;
    size_t         payload_len;
    int            compressed;

    int rc = csilk_grpc_frame_parse(buf, sizeof(buf), &payload, &payload_len, &compressed);
    assert(rc == 0);
    assert(payload_len == 4);
    assert(compressed == 0);
    assert(payload[0] == 0xAA);
    assert(payload[3] == 0xDD);

    /* Incomplete frame */
    rc = csilk_grpc_frame_parse(buf, 7, &payload, &payload_len, &compressed);
    assert(rc == -1);

    /* Too short */
    rc = csilk_grpc_frame_parse(buf, 3, &payload, &payload_len, &compressed);
    assert(rc == -1);

    /* Null args */
    rc = csilk_grpc_frame_parse(NULL, 10, &payload, &payload_len, NULL);
    assert(rc == -1);

    printf("test_frame_parse: PASS\n");
}

static void
test_frame_roundtrip(void)
{
    printf("Testing gRPC frame roundtrip...\n");

    uint8_t payload[100];
    for (int i = 0; i < 100; i++) {
        payload[i] = (uint8_t)i;
    }

    uint8_t buf[128];
    int     len = csilk_grpc_frame_encode_ex(payload, 100, 0, buf, sizeof(buf));
    assert(len == 105);

    const uint8_t* parsed;
    size_t         parsed_len;
    int            parsed_comp;
    int            rc = csilk_grpc_frame_parse(buf, 105, &parsed, &parsed_len, &parsed_comp);
    assert(rc == 0);
    assert(parsed_len == 100);
    assert(parsed_comp == 0);
    assert(memcmp(parsed, payload, 100) == 0);

    printf("test_frame_roundtrip: PASS\n");
}

static void
test_trailer_parse(void)
{
    printf("Testing gRPC trailer parse...\n");

    csilk_kv_t headers[] = {
        {"grpc-status",  "0"    },
        {"grpc-message", ""     },
        {"x-custom",     "value"},
    };

    int   status;
    char* message = NULL;

    int rc = csilk_grpc_trailer_parse(headers, 3, &status, &message);
    assert(rc == 0);
    assert(status == 0);
    if (message) {
        free(message);
        message = NULL;
    }

    /* Error trailer */
    csilk_kv_t error_headers[] = {
        {"grpc-status",  "14"         },
        {"grpc-message", "Unavailable"},
    };
    message = NULL;
    rc = csilk_grpc_trailer_parse(error_headers, 2, &status, &message);
    assert(rc == 0);
    assert(status == 14);
    assert(message != NULL);
    assert(strcmp(message, "Unavailable") == 0);
    free(message);

    /* Null args */
    rc = csilk_grpc_trailer_parse(NULL, 0, &status, NULL);
    assert(rc == -1);

    printf("test_trailer_parse: PASS\n");
}

static void
test_metadata_parse(void)
{
    printf("Testing gRPC metadata parse...\n");

    csilk_kv_t headers[] = {
        {"content-type", "application/grpc+proto"},
        {"grpc-timeout", "5s"                    },
        {"x-request-id", "abc123"                },
    };

    char*   content_type = NULL;
    int64_t timeout_ms = -1;

    int rc = csilk_grpc_metadata_parse(headers, 3, &content_type, &timeout_ms);
    assert(rc == 0);
    assert(content_type != NULL);
    assert(strcmp(content_type, "application/grpc+proto") == 0);
    assert(timeout_ms == 5000); /* 5s = 5000ms */
    free(content_type);

    /* Millisecond timeout */
    csilk_kv_t ms_headers[] = {
        {"grpc-timeout", "250m"},
    };
    content_type = NULL;
    timeout_ms = -1;
    rc = csilk_grpc_metadata_parse(ms_headers, 1, &content_type, &timeout_ms);
    assert(rc == 0);
    assert(timeout_ms == 250);

    /* No timeout */
    csilk_kv_t no_timeout[] = {
        {"content-type", "application/grpc"},
    };
    timeout_ms = -1;
    rc = csilk_grpc_metadata_parse(no_timeout, 1, &content_type, &timeout_ms);
    assert(rc == 0);
    assert(timeout_ms == -1);
    free(content_type);

    printf("test_metadata_parse: PASS\n");
}

/* --- Streaming tests --- */

static int     stream_msg_count;
static uint8_t stream_last_payload[256];
static size_t  stream_last_len;

static void
test_on_message(csilk_grpc_stream_t* stream, const uint8_t* data, size_t len, void* user_data)
{
    (void)stream;
    (void)user_data;
    stream_msg_count++;
    if (len <= sizeof(stream_last_payload)) {
        memcpy(stream_last_payload, data, len);
    }
    stream_last_len = len;
}

static void
test_streaming(void)
{
    printf("Testing gRPC streaming...\n");

    stream_msg_count = 0;

    csilk_grpc_stream_t* s =
        csilk_grpc_stream_new(CSILK_GRPC_STREAM_BIDI_STREAMING, test_on_message, NULL);
    assert(s != NULL);
    assert(csilk_grpc_stream_closed(s) == 0);

    uint8_t frame[] = {0x00, 0x00, 0x00, 0x00, 0x03, 0xAA, 0xBB, 0xCC};
    int     rc = csilk_grpc_stream_feed(s, frame, sizeof(frame));
    assert(rc == 0);
    assert(stream_msg_count == 1);
    assert(stream_last_len == 3);
    assert(stream_last_payload[0] == 0xAA);

    uint8_t frame2[] = {0x00, 0x00, 0x00, 0x00, 0x02, 0x11, 0x22};
    rc = csilk_grpc_stream_feed(s, frame2, sizeof(frame2));
    assert(rc == 0);
    assert(stream_msg_count == 2);

    rc = csilk_grpc_stream_send(s, NULL, 0, 1);
    assert(rc == 0);
    assert(csilk_grpc_stream_closed(s) == 0);

    csilk_grpc_stream_end(s);
    assert(csilk_grpc_stream_closed(s) == 1);
    csilk_grpc_stream_destroy(s);
    csilk_grpc_stream_destroy(NULL);

    csilk_grpc_stream_t* u = csilk_grpc_stream_new(CSILK_GRPC_STREAM_UNARY, NULL, NULL);
    assert(u != NULL);
    rc = csilk_grpc_stream_send(u, NULL, 0, 1);
    assert(rc == 0);
    assert(csilk_grpc_stream_closed(u) == 1);
    csilk_grpc_stream_destroy(u);

    printf("test_streaming: PASS\n");
}

static void
test_stream_concurrent(void)
{
    printf("Testing concurrent gRPC streams...\n");

#define NUM_STREAMS 8
    csilk_grpc_stream_t* streams[NUM_STREAMS];
    for (int i = 0; i < NUM_STREAMS; i++) {
        streams[i] = csilk_grpc_stream_new(CSILK_GRPC_STREAM_BIDI_STREAMING, NULL, NULL);
        assert(streams[i] != NULL);
    }

    for (int i = 0; i < NUM_STREAMS; i++) {
        uint8_t frame[] = {0x00, 0x00, 0x00, 0x00, 0x01, (uint8_t)i};
        int     rc = csilk_grpc_stream_feed(streams[i], frame, sizeof(frame));
        assert(rc == 0);
    }

    for (int i = 0; i < NUM_STREAMS; i++) {
        assert(csilk_grpc_stream_closed(streams[i]) == 0);
        csilk_grpc_stream_destroy(streams[i]);
    }
#undef NUM_STREAMS

    printf("test_stream_concurrent: PASS\n");
}

int
main(void)
{
    printf("csilk: gRPC protocol tests\n");
    printf("==========================================\n");

    test_frame_encode();
    test_frame_parse();
    test_frame_roundtrip();
    test_trailer_parse();
    test_metadata_parse();
    test_streaming();
    test_stream_concurrent();

    printf("==========================================\n");
    printf("All gRPC tests passed!\n");
    return 0;
}
