#ifndef GIN_INTERNAL_H
#define GIN_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

// SHA1
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} gin_sha1_ctx;

void gin_sha1_init(gin_sha1_ctx* context);
void gin_sha1_update(gin_sha1_ctx* context, const uint8_t* data, uint32_t len);
void gin_sha1_final(gin_sha1_ctx* context, uint8_t digest[20]);

// Base64
void gin_base64_encode(const uint8_t* src, size_t len, char* out);

// WebSocket Internal
void gin_ws_parse_frame(gin_ctx_t* c, const uint8_t* buf, size_t nread);

#endif
