#pragma once
/**
 * @file internal.h
 * @brief Internal framework primitives — umbrella header.
 *
 * This header aggregates all internal csilk framework modules.  Contents
 * are NOT part of the public API and may change without notice.  External
 * consumers should not rely on them.
 *
 * ## Included modules
 *   - **hash.h** — SHA-1, SHA-256, HMAC-SHA256.
 *   - **codec.h** — Base64, Base64URL, URL percent-decoding.
 *   - **ws_frame.h** — WebSocket frame parsing (RFC 6455).
 *   - **crypto_dispatch.h** — Weak-symbol stubs for crypto/cipher dispatch.
 *   - **mq_internal.h** — Message Queue types and internal API.
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include <csilk/core/sys_io.h>

#include "csilk/csilk.h"
#ifdef TEST_OOM
#include "csilk/test/test.h"
#endif

/* ================================================================
 * Sub-module headers
 * ================================================================ */

#include "csilk/core/hash.h"
#include "csilk/core/codec.h"
#include "csilk/core/ws_frame.h"
#include "csilk/core/crypto_dispatch.h"
#include "messaging/mq_internal.h"
#include "csilk/core/bounded_buf.h"

/* ================================================================
 * Portable secure memory zeroing
 * ================================================================ */

/**
 * @brief Portable replacement for explicit_bzero().
 *
 * Securely zeroes @p len bytes at @p ptr in a way the compiler is not
 * permitted to optimize away.  glibc/BSD provide explicit_bzero() natively;
 * platforms that lack it (e.g. macOS) fall back to memset_s() where available,
 * or a volatile-pointer memset otherwise.
 */
#if defined(__GLIBC__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) ||   \
    defined(__DragonFly__)
/* explicit_bzero() is provided by the C library; use it directly. */
#else
static inline void
csilk_explicit_bzero(void* ptr, size_t len)
{
#if defined(__STDC_LIB_EXT1__)
    memset_s(ptr, len, 0, len);
#else
    volatile unsigned char* p = (volatile unsigned char*)ptr;
    while (len--) {
        *p++ = 0;
    }
#endif
}
#define explicit_bzero(ptr, len) csilk_explicit_bzero((ptr), (len))
#endif

/* ================================================================
 * Shared constants (used across multiple translation units)
 * ================================================================ */

/** @brief UUID v4 string length (36 hex chars + 4 hyphens, no null). */
enum { CSILK_UUID_STR_LEN = 36 };

/** @brief Full buffer size for a UUID v4 string (str_len + null terminator). */
enum { CSILK_UUID_BUF_SIZE = 37 };
