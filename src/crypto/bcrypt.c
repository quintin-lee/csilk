/**
 * @file bcrypt.c
 * @brief bcrypt password hashing — Eksblowfish key schedule + OpenSSL-backed cryptographic core.
 *
 * Implements the OpenBSD bcrypt algorithm (the "$2a$" format) as described in:
 *   "A Future-Adaptable Password Scheme" — Niels Provos & David Mazières
 *
 * The algorithm:
 *   1. Initialise Blowfish P-array and S-boxes from the hex digits of π.
 *   2. Hash the password against the salt via the Eksblowfish key schedule
 *      (alternating encrypts with password and salt until P[0..17] and S are keyed).
 *   3. Run 2^cost iterations over the password and salt.
 *   4. Run Encrypt-Left rounds against the known ciphertext ("OrpheanBeholderScryDoubt")
 *      to produce 24 bytes of output.
 *   5. Encode salt + ciphertext in bcrypt base64 and format as $2a$XX$...
 *
 * OpenSSL Security Foundation:
 *   - RAND_bytes() / RAND_priv_bytes() for cryptographically secure random salt generation.
 *   - CRYPTO_memcmp() for constant-time comparison in csilk_bcrypt_verify.
 *   - OPENSSL_cleanse() to wipe passwords, salts, and expanded keys from the stack.
 *   - Fully re-entrant and thread-safe per-invocation cipher state.
 *
 * @note Passwords longer than 72 bytes are silently truncated, matching standard bcrypt.
 * @copyright MIT License
 */

#include "csilk/core/bcrypt.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <openssl/rand.h>
#include <openssl/crypto.h>

/* ================================================================
 *  Blowfish constants — P-array (18 × 32-bit) and S-boxes (4 × 256)
 *  seeded from the hexadecimal digits of π.
 * ================================================================ */

static const uint32_t pg[18] = {0x243F6A88U,
                                0x85A308D3U,
                                0x13198A2EU,
                                0x03707344U,
                                0xA4093822U,
                                0x299F31D0U,
                                0x082EFA98U,
                                0xEC4E6C89U,
                                0x452821E6U,
                                0x38D01377U,
                                0xBE5466CFU,
                                0x34E90C6CU,
                                0xC0AC29B7U,
                                0xC97C50DDU,
                                0x3F84D5B5U,
                                0xB5470917U,
                                0x9216D5D9U,
                                0x8979FB1BU};

/*
 * Full 1024-entry S-box table. Sourced from OpenBSD's passwd.c / blowfish.c
 * (public domain). Layout: 4 sub-boxes × 256 entries each.
 */
static const uint32_t sg_init[4][256] = {
#include "blowfish_sboxes.h"
};

/**
 * @brief Per-operation Eksblowfish state (thread-safe, stack-allocated).
 */
typedef struct {
    uint32_t P[18];
    uint32_t S[4][256];
} csilk_bcrypt_state_t;

/* ================================================================
 *  Bcrypt base64 alphabet
 *
 *  ./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789
 *  Index: 0='.', 1='/', 2='A' … 63='9'
 * ================================================================ */

static const char b64[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static int
b64_decode_char(char c)
{
    if (c == '.') {
        return 0;
    }
    if (c == '/') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 2);
    }
    if (c >= 'a' && c <= 'z') {
        return (int)(c - 'a' + 28);
    }
    if (c >= '0' && c <= '9') {
        return (int)(c - '0' + 54);
    }
    return -1;
}

/* ================================================================
 *  Blowfish encipher / decipher (thread-safe, operates on state)
 * ================================================================ */

static inline uint32_t
fo(const csilk_bcrypt_state_t* state, uint32_t x)
{
    uint8_t b0 = (uint8_t)(x & 0xFF);
    uint8_t b1 = (uint8_t)((x >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)((x >> 16) & 0xFF);
    uint8_t b3 = (uint8_t)((x >> 24) & 0xFF);
    return state->S[0][b0] ^ state->S[1][b1] ^ state->S[2][b2] ^ state->S[3][b3];
}

static void
blowfish_encipher(const uint32_t in[2], uint32_t out[2], csilk_bcrypt_state_t* state)
{
    uint32_t XL = in[0], XR = in[1];

    for (int i = 0; i < 16; i++) {
        XL ^= state->P[i];
        XR ^= fo(state, XL);
        uint32_t tmp = XL;
        XL = XR;
        XR = tmp;
    }

    out[0] = XL ^ state->P[16];
    out[1] = XR ^ state->P[17];
}

/* ================================================================
 *  Eksblowfish key schedule
 *
 *  Based on OpenBSD's Blowfish_expandstate algorithm:
 *  1. XOR password (key) into P-array
 *  2. For each P pair: XOR salt bytes, encrypt, store result
 *  3. For each S pair: XOR salt bytes, encrypt, store result
 * ================================================================ */

static void
eksblowfish_key_setup(const uint8_t         password[],
                      size_t                pwd_len,
                      const uint8_t         salt[],
                      size_t                salt_len,
                      csilk_bcrypt_state_t* state)
{
    uint32_t datal, datar;
    size_t   pwd_idx = 0, salt_idx = 0;

    memcpy(state->P, pg, sizeof(pg));
    memcpy(state->S, sg_init, sizeof(sg_init));

    /* Step 1: XOR password into P-array. */
    for (int i = 0; i < 18; i++) {
        datal = 0;
        for (int j = 0; j < 4; j++) {
            uint8_t byte;
            if (pwd_idx < pwd_len) {
                byte = password[pwd_idx++];
            } else {
                pwd_idx = 0;
                byte = password[pwd_idx++];
            }
            datal = (datal << 8) | byte;
        }
        state->P[i] ^= datal;
    }

    /* Step 2: Encrypt and key P-array with salt. */
    datal = 0;
    datar = 0;
    for (int i = 0; i < 18; i += 2) {
        uint32_t new_datal = 0;
        for (int j = 0; j < 4; j++) {
            uint8_t byte = salt[salt_idx++];
            if (salt_idx >= salt_len) {
                salt_idx = 0;
            }
            new_datal = (new_datal << 8) | byte;
        }
        uint32_t new_datar = 0;
        for (int j = 0; j < 4; j++) {
            uint8_t byte = salt[salt_idx++];
            if (salt_idx >= salt_len) {
                salt_idx = 0;
            }
            new_datar = (new_datar << 8) | byte;
        }

        uint32_t block[2] = {datal ^ new_datal, datar ^ new_datar};
        blowfish_encipher(block, block, state);
        datal = block[0];
        datar = block[1];
        state->P[i] = datal;
        state->P[i + 1] = datar;
    }

    /* Step 3: Encrypt and key S-boxes with salt. */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 256; j += 2) {
            uint32_t new_datal = 0;
            for (int k = 0; k < 4; k++) {
                uint8_t byte = salt[salt_idx++];
                if (salt_idx >= salt_len) {
                    salt_idx = 0;
                }
                new_datal = (new_datal << 8) | byte;
            }
            uint32_t new_datar = 0;
            for (int k = 0; k < 4; k++) {
                uint8_t byte = salt[salt_idx++];
                if (salt_idx >= salt_len) {
                    salt_idx = 0;
                }
                new_datar = (new_datar << 8) | byte;
            }

            uint32_t block[2] = {datal ^ new_datal, datar ^ new_datar};
            blowfish_encipher(block, block, state);
            state->S[i][j] = block[0];
            state->S[i][j + 1] = block[1];
        }
    }
}

/* ================================================================
 *  Bcrypt hash computation
 * ================================================================ */

static void
bcrypt_hash_internal(const uint8_t password[],
                     size_t        pwd_len,
                     const uint8_t salt[CSILK_BCRYPT_SALT_BYTES],
                     int           cost,
                     uint8_t       out[CSILK_BCRYPT_CIPHER_OUT])
{
    csilk_bcrypt_state_t state;

    /* Run Eksblowfish with 2^cost iterations. */
    for (int i = 0; i < (1 << cost); i++) {
        eksblowfish_key_setup(password, pwd_len, salt, CSILK_BCRYPT_SALT_BYTES, &state);
    }

    /* Final Encrypt-Left on "OrpheanBeholderScryDoubt". */
    static const uint8_t magic[24] = "OrpheanBeholderScryDoubt";
    uint32_t             blk_in[2], blk_out[2];

    blk_in[0] = ((uint32_t)magic[0] << 24) | ((uint32_t)magic[1] << 16) |
                ((uint32_t)magic[2] << 8) | magic[3];
    blk_in[1] = ((uint32_t)magic[4] << 24) | ((uint32_t)magic[5] << 16) |
                ((uint32_t)magic[6] << 8) | magic[7];
    blowfish_encipher(blk_in, blk_out, &state);
    out[0] = (blk_out[0] >> 24) & 0xFF;
    out[1] = (blk_out[0] >> 16) & 0xFF;
    out[2] = (blk_out[0] >> 8) & 0xFF;
    out[3] = blk_out[0] & 0xFF;
    out[4] = (blk_out[1] >> 24) & 0xFF;
    out[5] = (blk_out[1] >> 16) & 0xFF;
    out[6] = (blk_out[1] >> 8) & 0xFF;
    out[7] = blk_out[1] & 0xFF;

    blk_in[0] = ((uint32_t)magic[8] << 24) | ((uint32_t)magic[9] << 16) |
                ((uint32_t)magic[10] << 8) | magic[11];
    blk_in[1] = ((uint32_t)magic[12] << 24) | ((uint32_t)magic[13] << 16) |
                ((uint32_t)magic[14] << 8) | magic[15];
    blowfish_encipher(blk_in, blk_out, &state);
    out[8] = (blk_out[0] >> 24) & 0xFF;
    out[9] = (blk_out[0] >> 16) & 0xFF;
    out[10] = (blk_out[0] >> 8) & 0xFF;
    out[11] = blk_out[0] & 0xFF;
    out[12] = (blk_out[1] >> 24) & 0xFF;
    out[13] = (blk_out[1] >> 16) & 0xFF;
    out[14] = (blk_out[1] >> 8) & 0xFF;
    out[15] = blk_out[1] & 0xFF;

    blk_in[0] = ((uint32_t)magic[16] << 24) | ((uint32_t)magic[17] << 16) |
                ((uint32_t)magic[18] << 8) | magic[19];
    blk_in[1] = ((uint32_t)magic[20] << 24) | ((uint32_t)magic[21] << 16) |
                ((uint32_t)magic[22] << 8) | magic[23];
    blowfish_encipher(blk_in, blk_out, &state);
    out[16] = (blk_out[0] >> 24) & 0xFF;
    out[17] = (blk_out[0] >> 16) & 0xFF;
    out[18] = (blk_out[0] >> 8) & 0xFF;
    out[19] = blk_out[0] & 0xFF;
    out[20] = (blk_out[1] >> 24) & 0xFF;
    out[21] = (blk_out[1] >> 16) & 0xFF;
    out[22] = (blk_out[1] >> 8) & 0xFF;
    out[23] = blk_out[1] & 0xFF;

    OPENSSL_cleanse(&state, sizeof(state));
}

static void
encode_b64(const uint8_t* in, size_t len, char* out)
{
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = 0;
        int      remaining = 0;
        for (int j = 0; j < 3 && (i + j) < len; j++) {
            n = (n << 8) | in[i + j];
            remaining++;
        }
        n <<= (3 - remaining) * 8;

        for (int j = 0; j < 4; j++) {
            int idx = (n >> (18 - 6 * j)) & 0x3F;
            *out++ = b64[idx];
        }
    }
}

static void
decode_b64(const char* in, size_t len, uint8_t* out)
{
    size_t i = 0;
    while (i + 4 <= len) {
        int d[4];
        for (int j = 0; j < 4; j++) {
            d[j] = b64_decode_char(in[i + j]);
            if (d[j] < 0) {
                d[j] = 0; /* ignore padding '=' if present */
            }
        }
        uint32_t n = ((uint32_t)d[0] << 18) | ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 6) |
                     (uint32_t)d[3];
        out[0] = (n >> 16) & 0xFF;
        out[1] = (n >> 8) & 0xFF;
        out[2] = n & 0xFF;
        out += 3;
        i += 4;
    }
    /* Handle remaining 1-3 characters for incomplete groups. */
    if (i < len) {
        int remaining = (int)(len - i);
        int d[4] = {0, 0, 0, 0};
        for (int j = 0; j < remaining; j++) {
            d[j] = b64_decode_char(in[i + j]);
            if (d[j] < 0) {
                d[j] = 0;
            }
        }
        uint32_t n = ((uint32_t)d[0] << 18) | ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 6);
        out[0] = (n >> 16) & 0xFF;
        if (remaining >= 3) {
            out[1] = (n >> 8) & 0xFF;
        }
    }
}

/* ================================================================
 *  Public API
 * ================================================================ */

void
csilk_bcrypt_hash(const char* password, size_t len, int cost, char hash[CSILK_BCRYPT_HASH_LEN])
{
    /* Clamp cost to valid range. */
    if (cost < CSILK_BCRYPT_MIN_COST) {
        cost = CSILK_BCRYPT_MIN_COST;
    }
    if (cost > CSILK_BCRYPT_MAX_COST) {
        cost = CSILK_BCRYPT_MAX_COST;
    }

    /* Truncate password to 72 bytes. */
    uint8_t pwd_buf[72];
    memset(pwd_buf, 0, sizeof(pwd_buf));
    if (len > 72) {
        len = 72;
    }
    memcpy(pwd_buf, password, len);

    /* Generate a 16-byte random salt using OpenSSL RAND_bytes. */
    uint8_t salt[CSILK_BCRYPT_SALT_BYTES];
#ifdef TEST_OOM
    memset(salt, 0, sizeof(salt)); /* deterministic salt for tests */
#else
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        if (RAND_priv_bytes(salt, sizeof(salt)) != 1) {
            /* Salt generation failed — do not fall back to zeroed salt (CWE-330).
             * This is a fatal entropy failure; abort rather than compromise security. */
            abort();
        }
    }
#endif

    /* Run the hash. */
    uint8_t ciphertext[CSILK_BCRYPT_CIPHER_OUT];
    bcrypt_hash_internal(pwd_buf, len, salt, cost, ciphertext);

    /* Format: $2a$XX$<22-char-salt><32-char-ciphertext> */
    hash[0] = '$';
    hash[1] = '2';
    hash[2] = 'a';
    hash[3] = '$';
    hash[4] = '0' + (cost / 10);
    hash[5] = '0' + (cost % 10);
    hash[6] = '$';
    encode_b64(salt, CSILK_BCRYPT_SALT_BYTES, hash + 7);
    encode_b64(ciphertext, CSILK_BCRYPT_CIPHER_OUT, hash + 29);
    hash[CSILK_BCRYPT_HASH_LEN - 1] = '\0';

    OPENSSL_cleanse(pwd_buf, sizeof(pwd_buf));
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(ciphertext, sizeof(ciphertext));
}

int
csilk_bcrypt_verify(const char* password, size_t len, const char* hash)
{
    if (!hash || hash[0] != '$' || hash[1] != '2') {
        return -1;
    }

    /* Parse cost from hash string. */
    int cost = (hash[4] - '0') * 10 + (hash[5] - '0');
    if (cost < CSILK_BCRYPT_MIN_COST || cost > CSILK_BCRYPT_MAX_COST) {
        return -1;
    }

    /* Decode the 22-char salt (positions 7..28). */
    char salt_b64[23];
    strncpy(salt_b64, hash + 7, 22);
    salt_b64[22] = '\0';
    uint8_t salt[CSILK_BCRYPT_SALT_BYTES];
    decode_b64(salt_b64, 22, salt);

    /* Decode the 32-char checksum (positions 29..60). */
    char cksum_b64[33];
    strncpy(cksum_b64, hash + 29, 32);
    cksum_b64[32] = '\0';
    uint8_t expected[CSILK_BCRYPT_CIPHER_OUT];
    decode_b64(cksum_b64, 32, expected);

    /* Re-hash with the same salt and cost. */
    uint8_t pwd_buf[72];
    memset(pwd_buf, 0, sizeof(pwd_buf));
    if (len > 72) {
        len = 72;
    }
    memcpy(pwd_buf, password, len);
    uint8_t computed[CSILK_BCRYPT_CIPHER_OUT];
    bcrypt_hash_internal(pwd_buf, len, salt, cost, computed);

    /* Constant-time comparison using OpenSSL CRYPTO_memcmp. */
    int match = (CRYPTO_memcmp(computed, expected, CSILK_BCRYPT_CIPHER_OUT) == 0);

    OPENSSL_cleanse(pwd_buf, sizeof(pwd_buf));
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(expected, sizeof(expected));
    OPENSSL_cleanse(computed, sizeof(computed));

    return match ? 0 : -1;
}
