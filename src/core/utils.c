/**
 * @file utils.c
 * @brief SHA1 hashing and Base64 encoding utilities.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context_internal.h"
#include "csilk_internal.h"

#ifdef TEST_OOM
int g_oom_fail_after = -1;
int g_oom_count = 0;
#endif

/** @brief Rotate-left bitwise operation. */
#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/** @brief Core SHA1 transform function.
 * @param state In/out hash state array.
 * @param buffer 64-byte block to process. */
static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
  uint32_t a, b, c, d, e;
  uint32_t w[80];

  for (int i = 0; i < 16; i++) {
    w[i] = (buffer[i * 4] << 24) | (buffer[i * 4 + 1] << 16) |
           (buffer[i * 4 + 2] << 8) | (buffer[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];

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
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
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
void csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data,
                       size_t len) {
  uint32_t j = context->count[0];
  // context->count stores bytes, not bits, in the original implementation
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
  uint64_t total_bits =
      ((uint64_t)context->count[1] << 32 | context->count[0]) * 8;
  for (int i = 0; i < 8; i++)
    finalcount[i] = (uint8_t)((total_bits >> (56 - i * 8)) & 0xFF);

  uint32_t left = (context->count[0] % 64);
  uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
  uint8_t padding[128] = {0x80};
  csilk_sha1_update(context, padding, pad_len);
  csilk_sha1_update(context, finalcount, 8);

  for (int i = 0; i < 20; i++)
    digest[i] =
        (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
}

/* --- SHA256 Implementation --- */

#define ror(value, bits) (((value) >> (bits)) | ((value) << (32 - (bits))))
#define ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define sigma0(x) (ror(x, 2) ^ ror(x, 13) ^ ror(x, 22))
#define sigma1(x) (ror(x, 6) ^ ror(x, 11) ^ ror(x, 25))
#define gamma0(x) (ror(x, 7) ^ ror(x, 18) ^ ((x) >> 3))
#define gamma1(x) (ror(x, 17) ^ ror(x, 19) ^ ((x) >> 10))

static const uint32_t k256[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
  uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];

  for (int i = 0; i < 16; i++) {
    w[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16 |
           (uint32_t)data[i * 4 + 2] << 8 | (uint32_t)data[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
  }

  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];
  f = state[5];
  g = state[6];
  h = state[7];

  for (int i = 0; i < 64; i++) {
    t1 = h + sigma1(e) + ch(e, f, g) + k256[i] + w[i];
    t2 = sigma0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void csilk_sha256_init(csilk_sha256_ctx* context) {
  context->state[0] = 0x6a09e667;
  context->state[1] = 0xbb67ae85;
  context->state[2] = 0x3c6ef372;
  context->state[3] = 0xa54ff53a;
  context->state[4] = 0x510e527f;
  context->state[5] = 0x9b05688c;
  context->state[6] = 0x1f83d9ab;
  context->state[7] = 0x5be0cd19;
  context->count = 0;
}

void csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data,
                         size_t len) {
  uint32_t i, idx = (uint32_t)((context->count >> 3) & 0x3F);
  context->count += (uint64_t)len << 3;

  if (64 - idx <= len) {
    memcpy(context->buffer + idx, data, 64 - idx);
    sha256_transform(context->state, context->buffer);
    for (i = 64 - idx; i + 63 < len; i += 64) {
      sha256_transform(context->state, data + i);
    }
    idx = 0;
  } else {
    i = 0;
  }
  memcpy(context->buffer + idx, data + i, len - i);
}

void csilk_sha256_final(csilk_sha256_ctx* context, uint8_t digest[32]) {
  uint8_t finalcount[8];
  for (int i = 0; i < 8; i++)
    finalcount[i] = (uint8_t)((context->count >> (56 - i * 8)) & 0xFF);

  uint32_t left = (uint32_t)((context->count >> 3) % 64);
  uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
  uint8_t padding[128] = {0x80};
  csilk_sha256_update(context, padding, pad_len);
  csilk_sha256_update(context, finalcount, 8);

  for (int i = 0; i < 32; i++)
    digest[i] =
        (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
}

void csilk_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data,
                       size_t data_len, uint8_t out[32]) {
  csilk_sha256_ctx ctx;
  uint8_t k_ipad[64], k_opad[64], tk[32];

  if (key_len > 64) {
    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, key, key_len);
    csilk_sha256_final(&ctx, tk);
    key = tk;
    key_len = 32;
  }

  memset(k_ipad, 0, 64);
  memset(k_opad, 0, 64);
  memcpy(k_ipad, key, key_len);
  memcpy(k_opad, key, key_len);

  for (int i = 0; i < 64; i++) {
    k_ipad[i] ^= 0x36;
    k_opad[i] ^= 0x5c;
  }

  csilk_sha256_init(&ctx);
  csilk_sha256_update(&ctx, k_ipad, 64);
  csilk_sha256_update(&ctx, data, data_len);
  csilk_sha256_final(&ctx, out);

  csilk_sha256_init(&ctx);
  csilk_sha256_update(&ctx, k_opad, 64);
  csilk_sha256_update(&ctx, out, 32);
  csilk_sha256_final(&ctx, out);
}

/** @brief Base64 encoding lookup table. */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** @brief Encode raw bytes as a Base64 string (RFC 4648). */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out) {
  size_t i, j;
  for (i = 0, j = 0; i < len; i += 3) {
    uint32_t v = src[i] << 16;
    if (i + 1 < len) v |= src[i + 1] << 8;
    if (i + 2 < len) v |= src[i + 2];
    out[j++] = b64_table[(v >> 18) & 0x3F];
    out[j++] = b64_table[(v >> 12) & 0x3F];
    if (i + 1 < len)
      out[j++] = b64_table[(v >> 6) & 0x3F];
    else
      out[j++] = '=';
    if (i + 2 < len)
      out[j++] = b64_table[v & 0x3F];
    else
      out[j++] = '=';
  }
  out[j] = '\0';
}

/** @brief Encode raw bytes as a Base64URL string (RFC 4648). */
void csilk_base64url_encode(const uint8_t* src, size_t len, char* out) {
  csilk_base64_encode(src, len, out);
  for (char* p = out; *p; p++) {
    if (*p == '+')
      *p = '-';
    else if (*p == '/')
      *p = '_';
    else if (*p == '=') {
      *p = '\0';
      break;
    }
  }
}

/** @brief Decode a Base64URL string. */
int csilk_base64url_decode(const char* src, uint8_t* out) {
  size_t len = strlen(src);
  char* tmp = malloc(len + 5);
  if (!tmp) return -1;
  strcpy(tmp, src);
  for (size_t i = 0; i < len; i++) {
    if (tmp[i] == '-')
      tmp[i] = '+';
    else if (tmp[i] == '_')
      tmp[i] = '/';
  }
  while (len % 4) {
    tmp[len++] = '=';
  }
  tmp[len] = '\0';

  // Simple Base64 decode logic
  static const int8_t b64_rev_table[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
      52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
      -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
      -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
      41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};

  int decoded_len = 0;
  uint32_t v = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    if (tmp[i] == '=') break;
    int val = b64_rev_table[(uint8_t)tmp[i]];
    if (val < 0) {
      free(tmp);
      return -1;
    }
    v = (v << 6) | val;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[decoded_len++] = (uint8_t)((v >> bits) & 0xFF);
    }
  }
  free(tmp);
  return decoded_len;
}

/** @brief Generate a random UUID v4 string (8-4-4-4-12). */
void csilk_generate_uuid(char* buf) {
  uint8_t random[16];
  FILE* f = fopen("/dev/urandom", "rb");
  if (f) {
    if (fread(random, 1, 16, f) != 16) {
      /* Fallback to rand if urandom fails */
      for (int i = 0; i < 16; i++) random[i] = rand() & 0xFF;
    }
    fclose(f);
  } else {
    for (int i = 0; i < 16; i++) random[i] = rand() & 0xFF;
  }

  /* Set version (4) and variant (10xx) */
  random[6] = (random[6] & 0x0F) | 0x40;
  random[8] = (random[8] & 0x3F) | 0x80;

  sprintf(
      buf,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      random[0], random[1], random[2], random[3], random[4], random[5],
      random[6], random[7], random[8], random[9], random[10], random[11],
      random[12], random[13], random[14], random[15]);
}

void _csilk_hmac_sha256(csilk_ctx_t* c, const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len, uint8_t out[32]) {
  if (c && c->crypto_driver && c->crypto_driver->hmac_sha256) {
    c->crypto_driver->hmac_sha256(key, key_len, data, data_len, out);
  } else {
    csilk_hmac_sha256(key, key_len, data, data_len, out);
  }
}

void _csilk_generate_uuid(csilk_ctx_t* c, char buf[37]) {
  if (c && c->crypto_driver && c->crypto_driver->generate_uuid) {
    c->crypto_driver->generate_uuid(buf);
  } else {
    csilk_generate_uuid(buf);
  }
}
