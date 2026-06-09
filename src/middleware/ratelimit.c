/**
 * @file ratelimit.c
 * @brief Simple IP-based rate limiting middleware implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"

/**
/**
 * @brief Rate-limit tracking entry for a single IP address.
 *
 * Each entry stores the IP string, the request count within the current
 * window, the timestamp when the counting window started, and the timestamp
 * of the most recent request (used for LRU eviction when the table is full).
 */
typedef struct {
	char ip[46];	   /**< Client IP address string. */
	int count;	   /**< Request count in current window. */
	time_t last_reset; /**< Timestamp when the window started. */
	time_t last_seen;  /**< Timestamp of last request (for LRU eviction). */
} ip_entry_t;

static ip_entry_t ip_table[MAX_IP_ENTRIES];
static int ip_count = 0;
static time_t last_evict = 0;
static uv_mutex_t ratelimit_mutex;
static uv_once_t ratelimit_once = UV_ONCE_INIT;

/**
 * @brief Initialize the rate-limiting mutex (called once via uv_once).
 *
 * Creates the libuv mutex that protects the shared ip_table and ip_count
 * from concurrent access across worker threads.
 */
static void
init_ratelimit_mutex()
{
	uv_mutex_init(&ratelimit_mutex);
}

/**
 * @brief Compact the IP table by removing entries whose last_seen timestamp
 *        is older than WINDOW_SIZE seconds.
 *
 * Iterates over the table and compacts it in-place, updating ip_count to
 * reflect the number of remaining active entries.
 *
 * @param now  The current time to compare against last_seen.
 */
static void
evict_stale_entries(time_t now)
{
	int orig = ip_count;
	int write_idx = 0;
	for (int read_idx = 0; read_idx < ip_count; read_idx++) {
		if (now - ip_table[read_idx].last_seen <= WINDOW_SIZE) {
			if (write_idx != read_idx) {
				ip_table[write_idx] = ip_table[read_idx];
			}
			write_idx++;
		}
	}
	ip_count = write_idx;
	if (orig != ip_count) {
		CSILK_LOG_D("RateLimit: Evicted %d stale IP entry/entries. Active count: %d",
			    orig - ip_count,
			    ip_count);
	}
}

/**
 * @brief Evict the single oldest entry from the IP table (LRU policy).
 *
 * Scans the ip_table for the entry with the smallest last_seen value and
 * replaces it with the last entry in the table, decrementing ip_count.
 * Called when the table is full and a new IP needs to be tracked.
 */
static void
evict_oldest_entry(void)
{
	if (ip_count <= 0) {
		return;
	}
	int oldest = 0;
	for (int i = 1; i < ip_count; i++) {
		if (ip_table[i].last_seen < ip_table[oldest].last_seen) {
			oldest = i;
		}
	}
	CSILK_LOG_D("RateLimit: IP table full. Evicting oldest entry for IP '%s' (last seen: %ld)",
		    ip_table[oldest].ip,
		    (long)ip_table[oldest].last_seen);
	ip_table[oldest] = ip_table[ip_count - 1];
	ip_count--;
}

/**
 * @brief IP-based rate limiting middleware with sliding window.
 *
 * Tracks request counts per client IP address within a configurable window.
 * If the number of requests from a given IP exceeds the limit, a 429 Too
 * Many Requests response is sent (with a Retry-After header) and the
 * pipeline is aborted.
 *
 * The IP table is protected by a libuv mutex for thread safety. Stale
 * entries are periodically evicted to prevent unbounded memory growth.
 *
 * @param c     The request context.
 * @param limit Maximum number of requests allowed per IP within the
 *              WINDOW_SIZE (60-second) sliding window.
 *
 * @note The mutex is initialized once via uv_once on the first call.
 * @warning Rate limiting is per-worker-process. In multi-process
 *          deployments, each process maintains its own independent table
 *          unless an external store (Redis, etc.) is used instead.
 * @warning If csilk_get_client_ip() returns nullptr (e.g. in tests or certain
 *          proxy setups), the request is passed through without rate
 *          limiting.
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
			void _csilk_metrics_inc_rate_limit_blocks(void);
			_csilk_metrics_inc_rate_limit_blocks();
			csilk_set_header(c, "Retry-After", "60");
			csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
			csilk_abort(c);
		} else {
			csilk_next(c);
		}
		return;
	}

	/* Local in-memory rate limiting fallback */
	uv_once(&ratelimit_once, init_ratelimit_mutex);
	time_t now = time(nullptr);
	ip_entry_t* entry = nullptr;

	uv_mutex_lock(&ratelimit_mutex);

	/* Periodic eviction of stale entries prevents the table from filling
     with one-shot visitors. EVICT_INTERVAL (300 s) is intentionally
     longer than WINDOW_SIZE (60 s) to avoid excessive compaction. */
	if (now - last_evict > EVICT_INTERVAL) {
		evict_stale_entries(now);
		last_evict = now;
	}

	/* Linear scan for existing IP entry. The table is bounded by
     MAX_IP_ENTRIES (1024), so O(n) lookup is acceptable. */
	for (int i = 0; i < ip_count; i++) {
		if (strcmp(ip_table[i].ip, ip) == 0) {
			entry = &ip_table[i];
			break;
		}
	}

	if (!entry) {
		if (ip_count < MAX_IP_ENTRIES) {
			/* Slot available: create new entry, reset count to 1. */
			entry = &ip_table[ip_count++];
			strncpy(entry->ip, ip, sizeof(entry->ip) - 1);
			entry->ip[sizeof(entry->ip) - 1] = '\0';
			entry->count = 0;
			entry->last_reset = now;
		} else {
			/* Table full: evict the LRU entry (oldest last_seen). */
			evict_oldest_entry();
			entry = &ip_table[ip_count++];
			strncpy(entry->ip, ip, sizeof(entry->ip) - 1);
			entry->ip[sizeof(entry->ip) - 1] = '\0';
			entry->count = 0;
			entry->last_reset = now;
		}
	}

	/* Sliding window algorithm: if the window has expired since the last
     reset, start a new window with count=1. Otherwise increment the
     counter within the current window. This is a "fixed-window" variant
     that slides on the first request after expiry, avoiding the burst
     at the boundary of pure fixed-window schemes. */
	if (now - entry->last_reset > WINDOW_SIZE) {
		entry->count = 1;
		entry->last_reset = now;
	} else {
		entry->count++;
	}
	entry->last_seen = now;

	int local_count = entry->count;
	uv_mutex_unlock(&ratelimit_mutex);

	if (local_count > limit) {
		CSILK_LOG_W("RateLimit: [Local] Blocked request from IP %s: current count %d "
			    "exceeds limit %d",
			    ip,
			    local_count,
			    limit);
		void _csilk_metrics_inc_rate_limit_blocks(void);
		_csilk_metrics_inc_rate_limit_blocks();
		csilk_set_header(c, "Retry-After", "60");
		csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
		csilk_abort(c);
	} else {
		csilk_next(c);
	}
}
