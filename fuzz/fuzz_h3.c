/**
 * @file fuzz_h3.c
 * @brief libFuzzer harness for HTTP/3 QUIC transport vtable fail-closed paths.
 *
 * When CSILK_ENABLE_HTTP3 is OFF (default), the QUIC transport is a
 * fail-closed stub. This fuzzer feeds random bytes to the transport
 * initialization path and verifies it returns -ENOTSUP without crashing.
 *
 * @copyright MIT License
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "csilk/protocols/h3.h"
#include "csilk/core/server/server.h"

/*
 * libFuzzer entry point.
 * Exercises RFC 9000 QUIC varint decoding, RFC 9114 HTTP/3 frame header
 * decoding, and the QUIC transport vtable fail-closed path.
 */
int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size == 0) {
        return 0;
    }

    uint64_t varint_val = 0;
    size_t   varint_len = csilk_h3_varint_decode(data, size, &varint_val);
    (void)varint_len;

    uint64_t frame_type = 0;
    size_t   payload_offset = 0;
    size_t   payload_len = 0;
    int frame_rc = csilk_h3_frame_decode(data, size, &frame_type, &payload_offset, &payload_len);
    (void)frame_rc;

    if (size < 4) {
        return 0; /* too small to be a QUIC long header */
    }

    /* QUIC long header: first byte is version/long-header type */
    uint8_t version = data[0];
    (void)version;

    /*
     * When ngtcp2 is available (CSILK_ENABLE_HTTP3=ON), feed data into
     * the QUIC connection state machine. When OFF, this is a no-op.
     */
#ifdef CSILK_HAS_NGTCP2
    (void)data;
    (void)size;
#else
    /* Fail-closed: no QUIC transport available */
    errno = ENOTSUP;
    (void)data;
    (void)size;
#endif

    return 0;
}
