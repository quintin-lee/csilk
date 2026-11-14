/**
 * @file ratelimit.c
 * @brief Lockless IP-based rate limiting middleware implementation.
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"
#include "../core/ctx/ctx_internal.h"

/** @brief Maximum number of distinct IP addresses tracked concurrently. */
enum { MAX_IP_ENTRIES = 65536 };
/** @brief Rate limiting sliding window size in seconds. */
enum { WINDOW_SIZE = 60 };
/** @brief Bounded probe window for O(1) lookup, expiry reclamation, and LRU eviction. */
enum { MAX_PROBE_LEN = 512 };

/**
 * @brief Rate-limit tracking entry for a single IP address (Lockless).
 *
 * Slot state encoding:
 *   - 0             : empty
 *   - 1             : writer-exclusive (initializing or evicting)
 *   - >=2, even     : ready with (state - 2) >> 1 active key-comparison readers
 */
typedef struct {
    char             ip[46];     /**< Client IP address string. */
    _Atomic uint32_t count;      /**< Request count in current window. */
    _Atomic time_t   last_reset; /**< Timestamp when the window started. */
    _Atomic time_t   last_seen;  /**< Timestamp of last request. */
    _Atomic uint32_t state;      /**< Lockless state & reader-guard count. */
} atomic_ip_entry_t;

static atomic_ip_entry_t ip_table[MAX_IP_ENTRIES];
static _Atomic int       ip_saturation_policy = CSILK_RATE_SATURATION_EVICT_LRU;
static _Atomic time_t    ratelimit_time_offset = 0;

/* --- Keyed rate-limit table (composite / custom keys, csilk#9a) --- */

/** @brief Maximum number of distinct composite/custom keys tracked concurrently. */
enum { MAX_KEY_ENTRIES = 8192 };
/** @brief Maximum length of a stored composite/custom key (NUL-terminated). */
enum { MAX_KEY_LEN = 256 };

/**
 * @brief Rate-limit tracking entry for a composite/custom key (Lockless).
 *
 * Used by csilk_rate_limiter_middleware for CSILK_RATE_KEY_IP_PATH and
 * CSILK_RATE_KEY_CUSTOM keys, which are longer than a bare client IP.
 * @ref atomic_ip_entry_t is reserved for IP-only keys.
 */
typedef struct {
    char             key[MAX_KEY_LEN]; /**< Composite/custom key string. */
    _Atomic uint32_t count;            /**< Request count in current window. */
    _Atomic time_t   last_reset;       /**< Timestamp when the window started. */
    _Atomic time_t   last_seen;        /**< Timestamp of last request. */
    _Atomic uint64_t window_seconds;   /**< Window duration associated with this entry. */
    _Atomic uint32_t state;            /**< Lockless state & reader-guard count. */
} atomic_key_entry_t;

static atomic_key_entry_t key_table[MAX_KEY_ENTRIES];

/* Forward declaration for metrics counter — used in the rate-limit middleware */
extern void _csilk_metrics_inc_rate_limit_blocks(void);

static inline time_t
ratelimit_now(void)
{
    return time(NULL) + atomic_load_explicit(&ratelimit_time_offset, memory_order_relaxed);
}

static inline void
ratelimit_send_429(csilk_ctx_t* c, time_t retry_seconds)
{
    if (retry_seconds < 1) {
        retry_seconds = 1;
    }
    _csilk_metrics_inc_rate_limit_blocks();
    char retry_after[32];
    snprintf(retry_after, sizeof(retry_after), "%lld", (long long)retry_seconds);
    csilk_set_header(c, "Retry-After", retry_after);
    csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
    csilk_abort(c);
}

void
csilk_rate_limit_set_saturation_policy(csilk_rate_saturation_policy_t policy)
{
    atomic_store_explicit(&ip_saturation_policy, (int)policy, memory_order_release);
}

csilk_rate_saturation_policy_t
csilk_rate_limit_get_saturation_policy(void)
{
    return (csilk_rate_saturation_policy_t)atomic_load_explicit(&ip_saturation_policy,
                                                                memory_order_acquire);
}

CSILK_INTERNAL void
_csilk_ratelimit_reset_tables(void)
{
    for (uint32_t i = 0; i < MAX_IP_ENTRIES; i++) {
        atomic_store_explicit(&ip_table[i].state, 0, memory_order_relaxed);
        atomic_store_explicit(&ip_table[i].count, 0, memory_order_relaxed);
        atomic_store_explicit(&ip_table[i].last_reset, 0, memory_order_relaxed);
        atomic_store_explicit(&ip_table[i].last_seen, 0, memory_order_relaxed);
        ip_table[i].ip[0] = '\0';
    }
    for (uint32_t i = 0; i < MAX_KEY_ENTRIES; i++) {
        atomic_store_explicit(&key_table[i].state, 0, memory_order_relaxed);
        atomic_store_explicit(&key_table[i].count, 0, memory_order_relaxed);
        atomic_store_explicit(&key_table[i].last_reset, 0, memory_order_relaxed);
        atomic_store_explicit(&key_table[i].last_seen, 0, memory_order_relaxed);
        atomic_store_explicit(&key_table[i].window_seconds, 0, memory_order_relaxed);
        key_table[i].key[0] = '\0';
    }
    atomic_store_explicit(&ratelimit_time_offset, 0, memory_order_relaxed);
    atomic_store_explicit(
        &ip_saturation_policy, CSILK_RATE_SATURATION_EVICT_LRU, memory_order_release);
}

CSILK_INTERNAL void
_csilk_ratelimit_set_time_offset(time_t offset_secs)
{
    atomic_store_explicit(&ratelimit_time_offset, offset_secs, memory_order_release);
}

/** @brief djb2 hash of a key string, mapped into the key_table index range. */
static uint32_t
key_hash(const char* key)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*key++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % MAX_KEY_ENTRIES;
}

static bool
try_reclaim_key_slot(atomic_key_entry_t* slot,
                     const char*         key,
                     time_t              now,
                     uint64_t            window_seconds,
                     bool                require_expired)
{
    uint32_t expected = 2;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }

    if (require_expired) {
        time_t   seen = atomic_load_explicit(&slot->last_seen, memory_order_relaxed);
        uint64_t slot_win = atomic_load_explicit(&slot->window_seconds, memory_order_relaxed);
        if (slot_win < 1) {
            slot_win = 1;
        }
        if (now <= seen || (uint64_t)(now - seen) <= slot_win) {
            atomic_store_explicit(&slot->state, 2, memory_order_release);
            return false;
        }
    }

    snprintf(slot->key, sizeof(slot->key), "%s", key);
    atomic_init(&slot->count, 0);
    atomic_init(&slot->last_reset, now);
    atomic_init(&slot->last_seen, now);
    atomic_init(&slot->window_seconds, window_seconds);
    atomic_store_explicit(&slot->state, 2, memory_order_release);
    return true;
}

/**
 * @brief Find or atomically claim the lockless rate-limit slot for a key.
 *
 * Probes key_table starting at the key's hash index (bounded linear open
 * addressing). Reclaims expired slots along the probe path before consuming
 * fresh slots. When the probe window is saturated with active keys, applies
 * @p policy (evict oldest `last_seen`, fail open, or fail closed).
 */
static atomic_key_entry_t*
get_or_create_key_entry(const char*                    key,
                        time_t                         now,
                        uint64_t                       window_seconds,
                        csilk_rate_saturation_policy_t policy)
{
    uint32_t start_idx = key_hash(key);
    int32_t  expired_idx = -1;
    int32_t  lru_idx = -1;
    time_t   lru_seen = 0;
    uint32_t probe_len = MAX_PROBE_LEN < MAX_KEY_ENTRIES ? MAX_PROBE_LEN : MAX_KEY_ENTRIES;

    for (uint32_t i = 0; i < probe_len; i++) {
        uint32_t            idx = (start_idx + i) % MAX_KEY_ENTRIES;
        atomic_key_entry_t* slot = &key_table[idx];

        for (int spin = 0; spin < 64; spin++) {
            uint32_t s = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (s == 0) {
                /* Key does not exist beyond an empty slot. Prefer reclaiming an
                 * already-seen expired slot to keep probe chains compact. */
                if (expired_idx >= 0 &&
                    try_reclaim_key_slot(&key_table[expired_idx], key, now, window_seconds, true)) {
                    return &key_table[expired_idx];
                }
                uint32_t expected = 0;
                if (atomic_compare_exchange_strong_explicit(
                        &slot->state, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
                    snprintf(slot->key, sizeof(slot->key), "%s", key);
                    atomic_init(&slot->count, 0);
                    atomic_init(&slot->last_reset, now);
                    atomic_init(&slot->last_seen, now);
                    atomic_init(&slot->window_seconds, window_seconds);
                    atomic_store_explicit(&slot->state, 2, memory_order_release);
                    return slot;
                }
                continue;
            }

            if ((s & 1u) != 0) {
                /* Writer is initializing/evicting this slot; re-check briefly. */
                continue;
            }

            /* Acquire reader guard (+2) before inspecting non-atomic key string. */
            if (!atomic_compare_exchange_weak_explicit(
                    &slot->state, &s, s + 2, memory_order_acquire, memory_order_relaxed)) {
                continue;
            }

            bool     match = (strcmp(slot->key, key) == 0);
            time_t   seen = atomic_load_explicit(&slot->last_seen, memory_order_relaxed);
            uint64_t slot_win = atomic_load_explicit(&slot->window_seconds, memory_order_relaxed);
            if (slot_win < 1) {
                slot_win = 1;
            }
            if (match) {
                atomic_store_explicit(&slot->last_seen, now, memory_order_relaxed);
                atomic_store_explicit(&slot->window_seconds, window_seconds, memory_order_relaxed);
                atomic_fetch_sub_explicit(&slot->state, 2, memory_order_release);
                return slot;
            }

            if (expired_idx < 0 && now > seen && (uint64_t)(now - seen) > slot_win) {
                expired_idx = (int32_t)idx;
            }
            if (lru_idx < 0 || seen < lru_seen) {
                lru_idx = (int32_t)idx;
                lru_seen = seen;
            }

            atomic_fetch_sub_explicit(&slot->state, 2, memory_order_release);
            break;
        }
    }

    if (expired_idx >= 0 &&
        try_reclaim_key_slot(&key_table[expired_idx], key, now, window_seconds, true)) {
        return &key_table[expired_idx];
    }

    if (policy == CSILK_RATE_SATURATION_EVICT_LRU && lru_idx >= 0 &&
        try_reclaim_key_slot(&key_table[lru_idx], key, now, window_seconds, false)) {
        return &key_table[lru_idx];
    }

    return NULL;
}

/**
 * @brief Enforce a lockless in-memory rate-limit decision for an arbitrary key.
 *
 * Tracks request counts per composite/custom key within a configurable window.
 * If the count exceeds the limit a 429 Too Many Requests response is sent
 * (with a Retry-After header) and the pipeline is aborted. Saturation behavior
 * follows @p policy (evict LRU, fail open, or fail closed).
 *
 * @param c              The request context.
 * @param key            Throttle key string (must outlive the call).
 * @param limit          Maximum requests allowed per key within the window.
 * @param window_seconds Sliding window size in seconds (must be > 0).
 * @param policy         Saturation policy when no empty/expired slot is found.
 */
static void
_rate_limit_keyed_local(csilk_ctx_t*                   c,
                        const char*                    key,
                        int                            limit,
                        uint64_t                       window_seconds,
                        csilk_rate_saturation_policy_t policy)
{
    time_t now = ratelimit_now();
    if (window_seconds < 1) {
        window_seconds = 1;
    }

    atomic_key_entry_t* entry = get_or_create_key_entry(key, now, window_seconds, policy);
    if (!entry) {
        if (policy == CSILK_RATE_SATURATION_FAIL_CLOSED) {
            CSILK_LOG_W("RateLimit[Keyed]: key table saturated (fail-closed) for key %s", key);
            ratelimit_send_429(c, (time_t)window_seconds);
        } else {
            CSILK_LOG_W("RateLimit[Keyed]: key table saturated, skipping rate limiting for key %s",
                        key);
            csilk_next(c);
        }
        return;
    }

    time_t   reset = atomic_load(&entry->last_reset);
    uint32_t current_count = 0;

    if (now > reset && (uint64_t)(now - reset) > window_seconds) {
        if (atomic_compare_exchange_strong(&entry->last_reset, &reset, now)) {
            atomic_store(&entry->count, 1);
            current_count = 1;
        } else {
            current_count = atomic_fetch_add(&entry->count, 1) + 1;
        }
    } else {
        current_count = atomic_fetch_add(&entry->count, 1) + 1;
    }

    if ((int)current_count > limit) {
        CSILK_LOG_W("RateLimit[Keyed]: Blocked request for key %s: current count %u "
                    "exceeds limit %d",
                    key,
                    current_count,
                    limit);
        /* Retry-After = seconds remaining in the current window (at least 1).
         * Re-read last_reset so a window flip racing this request yields the
         * freshly-claimed window start. */
        time_t reset_now = atomic_load(&entry->last_reset);
        time_t retry = (time_t)window_seconds - (now - reset_now);
        ratelimit_send_429(c, retry);
    } else {
        csilk_next(c);
    }
}

/** @brief djb2 hash of an IP string, mapped into the ip_table index range. */
static uint32_t
ip_hash(const char* ip)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*ip++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % MAX_IP_ENTRIES;
}

static bool
try_reclaim_ip_slot(atomic_ip_entry_t* slot, const char* ip, time_t now, bool require_expired)
{
    uint32_t expected = 2;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }

    if (require_expired) {
        time_t seen = atomic_load_explicit(&slot->last_seen, memory_order_relaxed);
        if (now <= seen || (now - seen) <= WINDOW_SIZE) {
            atomic_store_explicit(&slot->state, 2, memory_order_release);
            return false;
        }
    }

    snprintf(slot->ip, sizeof(slot->ip), "%s", ip);
    atomic_init(&slot->count, 0);
    atomic_init(&slot->last_reset, now);
    atomic_init(&slot->last_seen, now);
    atomic_store_explicit(&slot->state, 2, memory_order_release);
    return true;
}

/** @brief Find or atomically claim the lockless rate-limit slot for an IP.
 *
 *  Probes ip_table starting at the IP's hash index (bounded linear open
 *  addressing). Reclaims expired slots before consuming fresh slots. When
 *  all probed slots hold active entries, applies @p policy (evict LRU, fail
 *  open, or fail closed).
 */
static atomic_ip_entry_t*
get_or_create_ip_entry(const char* ip, time_t now, csilk_rate_saturation_policy_t policy)
{
    uint32_t start_idx = ip_hash(ip);
    int32_t  expired_idx = -1;
    int32_t  lru_idx = -1;
    time_t   lru_seen = 0;
    uint32_t probe_len = MAX_PROBE_LEN < MAX_IP_ENTRIES ? MAX_PROBE_LEN : MAX_IP_ENTRIES;

    for (uint32_t i = 0; i < probe_len; i++) {
        uint32_t           idx = (start_idx + i) % MAX_IP_ENTRIES;
        atomic_ip_entry_t* slot = &ip_table[idx];

        for (int spin = 0; spin < 64; spin++) {
            uint32_t s = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (s == 0) {
                if (expired_idx >= 0 &&
                    try_reclaim_ip_slot(&ip_table[expired_idx], ip, now, true)) {
                    return &ip_table[expired_idx];
                }
                uint32_t expected = 0;
                if (atomic_compare_exchange_strong_explicit(
                        &slot->state, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
                    snprintf(slot->ip, sizeof(slot->ip), "%s", ip);
                    atomic_init(&slot->count, 0);
                    atomic_init(&slot->last_reset, now);
                    atomic_init(&slot->last_seen, now);
                    atomic_store_explicit(&slot->state, 2, memory_order_release);
                    return slot;
                }
                continue;
            }

            if ((s & 1u) != 0) {
                continue;
            }

            if (!atomic_compare_exchange_weak_explicit(
                    &slot->state, &s, s + 2, memory_order_acquire, memory_order_relaxed)) {
                continue;
            }

            bool   match = (strcmp(slot->ip, ip) == 0);
            time_t seen = atomic_load_explicit(&slot->last_seen, memory_order_relaxed);
            if (match) {
                atomic_store_explicit(&slot->last_seen, now, memory_order_relaxed);
                atomic_fetch_sub_explicit(&slot->state, 2, memory_order_release);
                return slot;
            }

            if (expired_idx < 0 && now > seen && (now - seen) > WINDOW_SIZE) {
                expired_idx = (int32_t)idx;
            }
            if (lru_idx < 0 || seen < lru_seen) {
                lru_idx = (int32_t)idx;
                lru_seen = seen;
            }

            atomic_fetch_sub_explicit(&slot->state, 2, memory_order_release);
            break;
        }
    }

    if (expired_idx >= 0 && try_reclaim_ip_slot(&ip_table[expired_idx], ip, now, true)) {
        return &ip_table[expired_idx];
    }

    if (policy == CSILK_RATE_SATURATION_EVICT_LRU && lru_idx >= 0 &&
        try_reclaim_ip_slot(&ip_table[lru_idx], ip, now, false)) {
        return &ip_table[lru_idx];
    }

    return NULL;
}

/**
 * @brief Enforce the lockless in-memory rate-limit decision for a given IP.
 *
 * Tracks request counts per client IP within a 60-second sliding window.
 * Reclaims expired slots and honours the configured saturation policy when
 * all probed slots contain active entries.
 *
 * @param c     The request context.
 * @param ip    Client IP string (must outlive the call).
 * @param limit Maximum number of requests allowed per IP within the 60-second window.
 */
CSILK_INTERNAL void
_csilk_rate_limit_local(csilk_ctx_t* c, const char* ip, int limit)
{
    time_t                         now = ratelimit_now();
    csilk_rate_saturation_policy_t policy = csilk_rate_limit_get_saturation_policy();
    atomic_ip_entry_t*             entry = get_or_create_ip_entry(ip, now, policy);

    if (!entry) {
        if (policy == CSILK_RATE_SATURATION_FAIL_CLOSED) {
            CSILK_LOG_W("RateLimit: [Local] IP table saturated (fail-closed) for IP %s", ip);
            ratelimit_send_429(c, WINDOW_SIZE);
        } else {
            CSILK_LOG_W("RateLimit: [Local] IP table saturated, skipping rate limiting for IP %s",
                        ip);
            csilk_next(c);
        }
        return;
    }

    time_t   reset = atomic_load(&entry->last_reset);
    uint32_t current_count = 0;

    if (now > reset && (now - reset) > WINDOW_SIZE) {
        if (atomic_compare_exchange_strong(&entry->last_reset, &reset, now)) {
            atomic_store(&entry->count, 1);
            current_count = 1;
        } else {
            current_count = atomic_fetch_add(&entry->count, 1) + 1;
        }
    } else {
        current_count = atomic_fetch_add(&entry->count, 1) + 1;
    }

    if ((int)current_count > limit) {
        CSILK_LOG_W("RateLimit: [Local] Blocked request from IP %s: current count %u "
                    "exceeds limit %d",
                    ip,
                    current_count,
                    limit);
        /* Retry-After = seconds remaining in the current window (at least 1).
         * Re-read last_reset so a window flip racing this request yields the
         * freshly-claimed window start. */
        time_t reset_now = atomic_load(&entry->last_reset);
        time_t retry = (time_t)WINDOW_SIZE - (now - reset_now);
        ratelimit_send_429(c, retry);
    } else {
        csilk_next(c);
    }
}

/**
 * @brief IP-based rate limiting middleware with lockless sliding window.
 *
 * Resolves the client IP from the live socket, uses the distributed storage
 * path when a storage driver is configured, and otherwise delegates to the
 * lockless in-memory path (see _csilk_rate_limit_local).
 *
 * @param c     The request context.
 * @param limit Maximum number of requests allowed per IP within the 60-second window.
 */
void
csilk_rate_limit_middleware(csilk_ctx_t* c, int limit)
{
    const char* ip = csilk_get_client_ip(c);
    if (!ip) {
        CSILK_LOG_T("RateLimit: Skipping rate limiting: client IP not available");
        csilk_next(c);
        return;
    }

    CSILK_LOG_T("RateLimit: Checking request %p from IP %s (limit: %d)", (void*)c, ip, limit);

    /* Distributed rate limiting using storage driver */
    if (c->storage_driver) {
        char key[128];
        snprintf(key, sizeof(key), "ratelimit:%s", ip);
        long long current_count = csilk_incr(c, key, WINDOW_SIZE);

        if (current_count >= 0) {
            if (current_count > limit) {
                CSILK_LOG_W("RateLimit: [Distributed] Blocked request from IP %s: current "
                            "count %lld exceeds limit %d",
                            ip,
                            current_count,
                            limit);
                _csilk_metrics_inc_rate_limit_blocks();
                /* Distributed store only returns the counter (no TTL); retry with
                 * the full window as a conservative (non-overstated) value. */
                char retry_after[32];
                snprintf(retry_after, sizeof(retry_after), "%lld", (long long)WINDOW_SIZE);
                csilk_set_header(c, "Retry-After", retry_after);
                csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
                csilk_abort(c);
            } else {
                csilk_next(c);
            }
            return;
        }
    }

    _csilk_rate_limit_local(c, ip, limit);
}

/**
 * @brief Rate-limit by an explicit caller-supplied key (csilk#9a).
 *
 * Delegates to the same lockless table as csilk_rate_limit_middleware,
 * but the throttle dimension is @p key (a user_id / tenant_id /
 * API-key) rather than the client IP.  The key is treated as an opaque
 * string and hashed into the same ip_table, so a user throttled here
 * and an IP throttled there share the same 60-second window budget
 * only when their strings collide (extremely rare for distinct ids).
 */
void
csilk_rate_limit_keyed(csilk_ctx_t* c, int limit, const char* key)
{
    if (!c || !key || !key[0]) {
        CSILK_LOG_T("RateLimit[Keyed]: no key supplied; skipping rate limiting");
        csilk_next(c);
        return;
    }
    _csilk_rate_limit_local(c, key, limit);
}

/**
 * @brief Keyed rate limiter middleware with configurable throttle key (csilk#9a).
 *
 * Auto-derives the throttle key from the request according to @p config->key_type:
 *   - CSILK_RATE_KEY_IP      : per-client-IP, tracked in the lockless key table
 *     (IP-only keys are short, so they use the generic keyed path here rather
 *     than the dedicated ip_table of csilk_rate_limit_middleware).
 *   - CSILK_RATE_KEY_IP_PATH : composite key of IP + request path, optionally
 *     filtered by config->match_paths (rate-limit only listed paths when set)
 *     and config->exclude_prefixes (skip paths starting with any prefix).
 *   - CSILK_RATE_KEY_CUSTOM  : key supplied by config->key_fn (e.g. user_id,
 *     tenant_id, API key).
 *
 * Uses the configured window_ms instead of the hardcoded 60-second window.
 * The composite/custom key is tracked in the lockless keyed table
 * (@ref key_table). Saturation behaviour is controlled by
 * @p config->saturation_policy (defaults to CSILK_RATE_SATURATION_EVICT_LRU).
 * The existing csilk_rate_limit_middleware is unaffected and remains
 * available.
 *
 * @param c      The request context.
 * @param config Limiter configuration (must remain valid for the call).
 */
void
csilk_rate_limiter_middleware(csilk_ctx_t* c, const csilk_rate_limiter_config_t* config)
{
    if (!c || !config) {
        csilk_next(c);
        return;
    }

    uint64_t window_ms = config->window_ms;
    if (window_ms < 1000) {
        /* Enforce a minimum 1-second window so the fixed-window logic is
         * meaningful in terms of time_t (second granularity). */
        window_ms = 1000;
    }
    uint64_t window_seconds = window_ms / 1000;
    if (window_seconds < 1) {
        window_seconds = 1;
    }

    int limit = config->limit;
    if (limit < 1) {
        limit = 1;
    }

    const char* ip = csilk_get_client_ip(c);
    if (!ip) {
        ip = "";
    }

    /* Derive the throttle key based on the configured key type. */
    switch (config->key_type) {
    case CSILK_RATE_KEY_IP:
        /* IP-only key: use the existing per-IP lockless path when window is 60s
         * and policy matches; otherwise route through the keyed table so
         * custom window_ms and saturation_policy take effect cleanly. */
        if (window_seconds == WINDOW_SIZE &&
            config->saturation_policy == csilk_rate_limit_get_saturation_policy()) {
            _csilk_rate_limit_local(c, ip, limit);
            return;
        }
        /* Otherwise fall through to the keyed table with the custom window/policy. */
        _rate_limit_keyed_local(c, ip, limit, window_seconds, config->saturation_policy);
        return;

    case CSILK_RATE_KEY_IP_PATH: {
        const char* path = csilk_get_path(c);
        if (!path) {
            path = "";
        }

        /* Skip paths that start with any excluded prefix. */
        if (config->exclude_prefixes) {
            for (const char** p = config->exclude_prefixes; *p; p++) {
                if (strncmp(path, *p, strlen(*p)) == 0) {
                    CSILK_LOG_T("RateLimit[Keyed]: path %s excluded, skipping rate limiting", path);
                    csilk_next(c);
                    return;
                }
            }
        }

        /* When match_paths is set, only rate-limit listed paths. */
        if (config->match_paths) {
            int matched = 0;
            for (const char** p = config->match_paths; *p; p++) {
                if (strcmp(path, *p) == 0) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                CSILK_LOG_T("RateLimit[Keyed]: path %s not in match_paths, skipping rate limiting",
                            path);
                csilk_next(c);
                return;
            }
        }

        char key[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "%s:%s", ip, path);
        _rate_limit_keyed_local(c, key, limit, window_seconds, config->saturation_policy);
        return;
    }

    case CSILK_RATE_KEY_CUSTOM:
    default:
        if (!config->key_fn) {
            CSILK_LOG_T("RateLimit[Keyed]: custom key type without key_fn; skipping rate limiting");
            csilk_next(c);
            return;
        }
        const char* custom = config->key_fn(c);
        if (!custom || !custom[0]) {
            CSILK_LOG_T("RateLimit[Keyed]: key_fn returned no key; skipping rate limiting");
            csilk_next(c);
            return;
        }
        _rate_limit_keyed_local(c, custom, limit, window_seconds, config->saturation_policy);
        return;
    }
}
