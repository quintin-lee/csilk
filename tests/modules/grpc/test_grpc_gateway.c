#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_grpc_frame_encode()
{
    printf("Testing gRPC 5-byte Frame Encoding...\n");

    const uint8_t payload[] = {0x08, 0x2a, 0x12, 0x05, 'c', 's', 'i', 'l', 'k'};
    uint8_t       frame_buf[32] = {0};

    int frame_len = csilk_grpc_frame_encode(payload, sizeof(payload), frame_buf, sizeof(frame_buf));
    assert(frame_len == (int)(sizeof(payload) + 5));

    // Compressed flag = 0
    assert(frame_buf[0] == 0x00);

    // Big-Endian length = 9
    assert(frame_buf[1] == 0x00);
    assert(frame_buf[2] == 0x00);
    assert(frame_buf[3] == 0x00);
    assert(frame_buf[4] == 0x09);

    // Payload match
    assert(memcmp(frame_buf + 5, payload, sizeof(payload)) == 0);

    printf("test_grpc_frame_encode: PASS\n");
}

static void
test_grpc_gateway_middleware()
{
    printf("Testing gRPC Gateway middleware...\n");

    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_set_request_header(ctx, "content-type", "application/grpc+json");
    csilk_grpc_gateway_middleware(ctx);

    const char* res_ct = csilk_get_response_header(ctx, "content-type");
    const char* grpc_status = csilk_get_response_header(ctx, "grpc-status");

    assert(res_ct != NULL && strcmp(res_ct, "application/grpc") == 0);
    assert(grpc_status != NULL && strcmp(grpc_status, "0") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_grpc_gateway_middleware: PASS\n");
}

int
main()
{
    test_grpc_frame_encode();
    test_grpc_gateway_middleware();
    printf("All gRPC Gateway tests passed successfully!\n");
    return 0;
}
