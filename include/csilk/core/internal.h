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
 *   - **Framework Constants** — Compile-time configuration limits.
 * @copyright MIT License
 */

#ifndef CSILK_INTERNAL_H
#define CSILK_INTERNAL_H

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
 * Framework Constants
 * ================================================================ */

/** @brief Maximum number of children per router tree node. */
enum { CSILK_MAX_CHILDREN = 128 };

/** @brief Maximum number of distinct IP addresses tracked concurrently by the
 * rate limiter. */
enum { MAX_IP_ENTRIES = 1024 };

/** @brief Rate limiting sliding window size in seconds. */
enum { WINDOW_SIZE = 60 };

/** @brief Interval at which ratelimit stale entries are garbage-collected. */
enum { EVICT_INTERVAL = 300 };

/** @brief Gzip compression chunk size in bytes. */
enum { CSILK_GZIP_CHUNK = 16384 };

/** @brief Minimum content length to enable gzip compression. */
enum { CSILK_GZIP_MIN_LENGTH = 1024 };

/** @brief Maximum number of metrics entries tracked. */
enum { CSILK_METRICS_MAX_ENTRIES = 1024 };

/** @brief Number of histogram buckets for latency metrics. */
enum { CSILK_METRICS_BUCKET_COUNT = 6 };

/** @brief Maximum multipart form part headers. */
enum { CSILK_MAX_PART_HEADERS = 32 };

/** @brief Maximum multipart form part name length. */
enum { CSILK_MAX_PART_NAME = 128 };

/** @brief Maximum multipart form part filename length. */
enum { CSILK_MAX_PART_FILENAME = 256 };

/** @brief Default session TTL in seconds. */
enum { SESSION_TTL = 3600 };

/** @brief CPU cache line size hint for arena alignment. */
enum { CSILK_CACHE_LINE_SIZE = 64 };

/** @brief UUID v4 string length (36 hex chars + 4 hyphens, no null). */
enum { CSILK_UUID_STR_LEN = 36 };

/** @brief Full buffer size for a UUID v4 string (str_len + null terminator). */
enum { CSILK_UUID_BUF_SIZE = 37 };

/** @brief Maximum number of route groups per app. */
enum { CSILK_MAX_GROUPS = 32 };

/** @brief Maximum number of static file routes per app. */
enum { CSILK_MAX_STATIC = 32 };

/** @brief Default HTTP listen port. */
enum { CSILK_DFL_PORT = 8080 };

/** @brief Initial capacity for group middleware array. */
enum { CSILK_GROUP_MW_INIT_CAP = 4 };

/** @brief Maximum workflow steps per definition. */
enum { MAX_WORKFLOW_STEPS = 1000 };

/** @brief Maximum permission rules per simple driver. */
enum { MAX_RULES = 128 };

#endif
