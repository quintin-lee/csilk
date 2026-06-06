#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

static int messages_received = 0;

static void
on_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
	(void)c;
	printf("Received WS message: %s (opcode: %d, len: %zu)\n", (char*)payload, opcode, len);
	assert(strcmp((char*)payload, "hello") == 0);
	assert(opcode == 1);
	assert(len == 5);
	messages_received++;
}

static void
on_any_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
	(void)c;
	(void)payload;
	(void)len;
	messages_received++;
}

static void
test_handshake()
{
	printf("Testing WebSocket Handshake...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_set_request_header(ctx, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");

	csilk_ws_handshake(ctx);
	assert(csilk_get_status(ctx) == CSILK_STATUS_SWITCHING_PROTOCOLS);
	assert(csilk_is_websocket(ctx) == 1);

	assert(csilk_test_ctx_count_response_headers(
		   ctx, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 1);
	assert(csilk_test_ctx_count_response_headers(ctx, "Upgrade", "websocket") == 1);

	csilk_test_ctx_free(ctx);
	printf("Handshake test passed!\n");
}

static void
test_handshake_missing_key()
{
	printf("Testing WebSocket Handshake missing key...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_ws_handshake(ctx);
	assert(csilk_get_status(ctx) == CSILK_STATUS_BAD_REQUEST);
	csilk_test_ctx_free(ctx);
	printf("Handshake missing key test passed!\n");
}

static void
test_parse_frame_basic()
{
	printf("Testing WebSocket frame parse basic...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_set_on_ws_message(ctx, on_message);
	uint8_t frame[] = {0x81, 0x85, 0x00, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
	csilk_ws_parse_frame(ctx, frame, sizeof(frame));
	assert(messages_received == 1);
	messages_received = 0;
	csilk_test_ctx_free(ctx);
	printf("Basic frame parse passed!\n");
}

static void
test_parse_frame_unmasked()
{
	printf("Testing WebSocket frame parse unmasked...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_set_on_ws_message(ctx, on_message);
	uint8_t frame[] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
	csilk_ws_parse_frame(ctx, frame, sizeof(frame));
	assert(messages_received == 1);
	messages_received = 0;
	csilk_test_ctx_free(ctx);
	printf("Unmasked frame parse passed!\n");
}

static void
test_parse_frame_fragmented()
{
	printf("Testing WebSocket fragmented frame parse...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	messages_received = 0;
	csilk_set_on_ws_message(ctx, on_message);
	uint8_t frame[] = {0x01, 0x85, 0x00, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
	csilk_ws_parse_frame(ctx, frame, sizeof(frame));
	assert(messages_received == 1);
	messages_received = 0;
	csilk_test_ctx_free(ctx);
	printf("Fragmented frame parse passed!\n");
}

static void
test_parse_frame_binary()
{
	printf("Testing WebSocket binary frame parse...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_set_on_ws_message(ctx, on_any_message);
	messages_received = 0;
	uint8_t frame[] = {0x82, 0x84, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
	csilk_ws_parse_frame(ctx, frame, sizeof(frame));
	assert(messages_received == 1);
	messages_received = 0;
	csilk_test_ctx_free(ctx);
	printf("Binary frame parse passed!\n");
}

static void
test_parse_frame_medium_payload()
{
	printf("Testing WebSocket frame parse medium payload (>125B)...\n");
	size_t payload_len = 200;
	size_t frame_len = 2 + 2 + 4 + payload_len;
	uint8_t* frame = calloc(1, frame_len);
	assert(frame != nullptr);

	frame[0] = 0x81;
	frame[1] = 0xFE; // masked, 126 extended
	frame[2] = (uint8_t)((payload_len >> 8) & 0xFF);
	frame[3] = (uint8_t)(payload_len & 0xFF);

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	messages_received = 0;
	csilk_set_on_ws_message(ctx, on_any_message);
	csilk_ws_parse_frame(ctx, frame, frame_len);
	assert(messages_received == 1);
	messages_received = 0;
	csilk_test_ctx_free(ctx);
	free(frame);
	printf("Medium payload parse passed!\n");
}

static void
test_parse_frame_ping_pong_close()
{
	printf("Testing WebSocket ping/pong/close frames...\n");

	uint8_t ping[] = {0x89, 0x00};
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_set_on_ws_message(ctx, nullptr);
	csilk_ws_parse_frame(ctx, ping, sizeof(ping));

	uint8_t pong[] = {0x8A, 0x00};
	csilk_ws_parse_frame(ctx, pong, sizeof(pong));

	uint8_t close[] = {0x88, 0x00};
	csilk_ws_parse_frame(ctx, close, sizeof(close));

	csilk_test_ctx_free(ctx);
	printf("Ping/pong/close frames parse passed!\n");
}

static void
test_parse_frame_truncated()
{
	printf("Testing WebSocket truncated frames...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	uint8_t short_frame[] = {0x81};
	csilk_ws_parse_frame(ctx, short_frame, sizeof(short_frame));

	uint8_t partial_frame[] = {0x81, 0x85, 0x00};
	csilk_ws_parse_frame(ctx, partial_frame, sizeof(partial_frame));

	csilk_test_ctx_free(ctx);
	printf("Truncated frame parse passed!\n");
}

static void
test_ws_send_null()
{
	printf("Testing csilk_ws_send with nullptr context/client...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_ws_send(nullptr, (uint8_t*)"hi", 2, 1);
	csilk_ws_send(ctx, (uint8_t*)"hi", 2, 1);
	csilk_test_ctx_free(ctx);
	printf("csilk_ws_send null safe passed!\n");
}

static void
test_parse_frame_large_payload()
{
	printf("Testing WebSocket large payload (>65535B)...\n");
	size_t payload_len = 70000;
	size_t frame_len = 2 + 8 + 4 + payload_len;
	uint8_t* frame = calloc(1, frame_len);
	assert(frame != nullptr);

	frame[0] = 0x81;
	frame[1] = 0xFF; // masked, 127 extended
	for (int i = 0; i < 8; i++) {
		frame[2 + i] = (uint8_t)((payload_len >> (56 - i * 8)) & 0xFF);
	}

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	messages_received = 0;
	csilk_set_on_ws_message(ctx, on_any_message);
	csilk_ws_parse_frame(ctx, frame, frame_len);
	assert(messages_received == 1);
	messages_received = 0;
	csilk_test_ctx_free(ctx);
	free(frame);
	printf("Large payload parse passed!\n");
}

static void
test_ws_close_normal()
{
	printf("Testing WebSocket close normal...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_ws_close(ctx, 1000, "normal closure");
	/* The frame should have been written, no crash */

	csilk_test_ctx_free(ctx);
	printf("WebSocket close normal passed!\n");
}

static void
test_ws_close_no_reason()
{
	printf("Testing WebSocket close without reason...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_ws_close(ctx, 1000, nullptr);

	csilk_test_ctx_free(ctx);
	printf("WebSocket close no reason passed!\n");
}

static void
test_ws_close_null()
{
	printf("Testing WebSocket close nullptr context...\n");
	csilk_ws_close(nullptr, 1000, "test");
	printf("WebSocket close null passed!\n");
}

static void
test_ws_close_handshake()
{
	printf("Testing WebSocket close handshake...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	/* No real client handle — csilk_ws_parse_frame will still detect the
     * close opcode and return without sending (csilk_ws_close checks
     * _internal_client and skips if nullptr). The payload is freed internally. */
	uint8_t close_frame[] = {0x88, 0x02, 0x03, 0xE8};
	csilk_ws_parse_frame(ctx, close_frame, sizeof(close_frame));

	csilk_test_ctx_free(ctx);
	printf("WebSocket close handshake test passed!\n");
}

int
main()
{
	test_handshake();
	test_handshake_missing_key();
	test_parse_frame_basic();
	test_parse_frame_unmasked();
	test_parse_frame_fragmented();
	test_parse_frame_binary();
	test_parse_frame_medium_payload();
	test_parse_frame_ping_pong_close();
	test_parse_frame_truncated();
	test_ws_send_null();
	test_parse_frame_large_payload();
	test_ws_close_normal();
	test_ws_close_no_reason();
	test_ws_close_null();
	test_ws_close_handshake();
	printf("test_ws: ALL PASSED\n");
	return 0;
}
