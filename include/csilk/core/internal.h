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
 * Shared constants (used across multiple translation units)
 * ================================================================ */

/** @brief UUID v4 string length (36 hex chars + 4 hyphens, no null). */
enum { CSILK_UUID_STR_LEN = 36 };

/** @brief Full buffer size for a UUID v4 string (str_len + null terminator). */
enum { CSILK_UUID_BUF_SIZE = 37 };
