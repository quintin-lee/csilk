/**
 * @file utils.c
 * @brief SHA1 hashing and Base64 encoding utilities.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/** @brief SHA1 hashing context (local definition for WebSocket handshake). */
typedef struct {
    uint32_t state[5];   /**< Intermediate hash state. */
    uint32_t count[2];   /**< Message length counter. */
    uint8_t buffer[64];   /**< Data block buffer. */
} csilk_sha1_ctx;

/** @brief Rotate-left bitwise operation. */
#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/** @brief Core SHA1 transform function.
 * @param state In/out hash state array.
 * @param buffer 64-byte block to process. */
static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    uint32_t w[80];
    
    for (int i = 0; i < 16; i++) {
        w[i] = (buffer[i*4] << 24) | (buffer[i*4+1] << 16) | (buffer[i*4+2] << 8) | (buffer[i*4+3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

/** @brief Initialize a SHA1 hashing context. */
void csilk_sha1_init(csilk_sha1_ctx* context) {
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}

/** @brief Feed data into the SHA1 hashing context. */
void csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data, size_t len) {
    uint32_t i, j;
    j = context->count[0];
    if ((context->count[0] += (uint32_t)len) < j) context->count[1]++;
    
    j %= 64;
    uint32_t fill = 64 - j;
    size_t i_sz;
    
    if (len >= fill) {
        memcpy(context->buffer + j, data, fill);
        sha1_transform(context->state, context->buffer);
        for (i_sz = fill; i_sz + 63 < len; i_sz += 64) {
            sha1_transform(context->state, data + i_sz);
        }
        j = 0;
    } else {
        i_sz = 0;
    }
    memcpy(context->buffer + j, data + i_sz, len - i_sz);
}

/** @brief Finalize SHA1 hash and produce the 20-byte digest. */
void csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20]) {
    uint8_t finalcount[8];
    uint64_t total_bits = (uint64_t)context->count[0] * 8;
    for (int i = 0; i < 8; i++) finalcount[i] = (uint8_t)((total_bits >> (56 - i * 8)) & 0xFF);
    
    uint32_t left = (context->count[0] % 64);
    uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
    uint8_t padding[128] = {0x80};
    csilk_sha1_update(context, padding, pad_len);
    csilk_sha1_update(context, finalcount, 8);
    
    for (int i = 0; i < 20; i++) digest[i] = (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
}

/** @brief Base64 encoding lookup table. */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** @brief Encode raw bytes as a Base64 string (RFC 4648). */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out) {
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t v = src[i] << 16;
        if (i + 1 < len) v |= src[i + 1] << 8;
        if (i + 2 < len) v |= src[i + 2];
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        if (i + 1 < len) out[j++] = b64_table[(v >> 6) & 0x3F]; else out[j++] = '=';
        if (i + 2 < len) out[j++] = b64_table[v & 0x3F]; else out[j++] = '=';
    }
    out[j] = '\0';
}
