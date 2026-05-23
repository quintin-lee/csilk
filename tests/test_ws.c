#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gin.h"
#include "gin_internal.h"

void on_message(gin_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    printf("Received WS message: %s (opcode: %d)\n", (char*)payload, opcode);
    assert(strcmp((char*)payload, "hello") == 0);
    assert(opcode == 1);
}

int main() {
    gin_ctx_t ctx = {0};
    gin_set_request_header(&ctx, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    
    printf("Testing WebSocket Handshake...\n");
    gin_ws_handshake(&ctx);
    
    assert(ctx.response.status == 101);
    assert(ctx.is_websocket == 1);
    
    // Check Accept Key (Calculated from RFC example)
    gin_header_t* h = ctx.response.headers;
    int found = 0;
    while(h) {
        if (strcmp(h->key, "Sec-WebSocket-Accept") == 0) {
            printf("Actual Accept Key: %s\n", h->value);
            assert(strcmp(h->value, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
            found = 1;
        }
        h = h->next;
    }
    assert(found);

    printf("Testing WebSocket Frame Parsing...\n");
    ctx.on_ws_message = on_message;
    
    // "hello" masked with 0x00000000 (simplified)
    uint8_t frame[] = {0x81, 0x85, 0x00, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
    gin_ws_parse_frame(&ctx, frame, sizeof(frame));

    gin_ctx_cleanup(&ctx);
    printf("test_ws: PASS\n");
    return 0;
}
