/**
 * @file sha1.c
 * @brief SHA-1 hash implementation (RFC 3174) for WebSocket handshake.
 *
 * Implements the SHA-1 cryptographic hash function producing a 160-bit
 * (20-byte) digest.  Used exclusively for WebSocket handshake key
 * verification per RFC 6455.
 *
 * @warning SHA-1 is cryptographically broken.  Do NOT use for security-
 *          critical purposes.  This implementation exists only for
 *          WebSocket protocol compliance.
 * @copyright MIT License
 */

#include <openssl/sha.h>
#include <stdint.h>
#include <string.h>

#include "csilk/core/hash.h"

void
csilk_sha1_init(csilk_sha1_ctx* context)
{
    if (context) {
        _Static_assert(sizeof(csilk_sha1_ctx) >= sizeof(SHA_CTX), "csilk_sha1_ctx too small");
        SHA1_Init((SHA_CTX*)context);
    }
}

void
csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data, size_t len)
{
    if (context && data && len > 0) {
        SHA1_Update((SHA_CTX*)context, data, len);
    }
}

void
csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20])
{
    if (context && digest) {
        SHA1_Final(digest, (SHA_CTX*)context);
    }
}
