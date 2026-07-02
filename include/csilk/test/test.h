/**
 * @file csilk_test.h
 * @brief Test helpers for out-of-memory (OOM) simulation and mock allocation.
 *
 * When compiled with -DTEST_OOM, the standard malloc/calloc/realloc calls
 * are replaced with instrumented versions that can be made to fail after a
 * configurable number of allocations.  This allows tests to verify that the
 * framework handles allocation failures gracefully.
 *
 * Usage:
 * @code
 *   // In test setup:
 *   g_oom_fail_after = 5;   // fail on the 5th allocation
 *   g_oom_count = 0;        // reset counter
 *
 *   // Run code under test — the 5th malloc/calloc/realloc returns nullptr.
 * @endcode
 *
 * Without -DTEST_OOM, this header is a no-op (macros expand to the standard
 * functions).
 * @copyright MIT License
 */

#ifndef CSILK_TEST_H
#define CSILK_TEST_H

#include <stdlib.h>

#ifdef TEST_OOM

/**
 * @brief Global: trigger allocation failure after this many calls.
 *
 * When >= 0, allocations at or beyond this count return nullptr.
 * Set to -1 to disable OOM simulation.
 */
extern int g_oom_fail_after;

/**
 * @brief Global: number of allocations attempted so far.
 *
 * Incremented on every call to the mock allocation functions.
 * Reset to 0 at the start of each test case.
 */
extern int g_oom_count;

/**
 * @brief Mock malloc that fails after g_oom_fail_after calls.
 *
 * @param size  Number of bytes to allocate.
 * @return Pointer to allocated memory, or nullptr if the failure threshold
 *         has been reached.
 */
static inline void*
csilk_test_malloc(size_t size)
{
    if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) {
        return nullptr;
    }
    g_oom_count++;
    return malloc(size);
}

/**
 * @brief Mock calloc that fails after g_oom_fail_after calls.
 *
 * @param nmemb  Number of elements.
 * @param size   Size of each element.
 * @return Pointer to zero-initialised allocated memory, or nullptr if the
 *         failure threshold has been reached.
 */
static inline void*
csilk_test_calloc(size_t nmemb, size_t size)
{
    if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) {
        return nullptr;
    }
    g_oom_count++;
    return calloc(nmemb, size);
}

/**
 * @brief Mock realloc that fails after g_oom_fail_after calls.
 *
 * @param ptr   Previously allocated pointer (may be nullptr).
 * @param size  New size in bytes.
 * @return Pointer to resized memory, or nullptr if the failure threshold
 *         has been reached.
 */
static inline void*
csilk_test_realloc(void* ptr, size_t size)
{
    if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) {
        return nullptr;
    }
    g_oom_count++;
    return realloc(ptr, size);
}

/**
 * @name OOM Mock Macro Overrides
 * When TEST_OOM is defined, these macros redirect the standard allocation
 * functions to the OOM-testing wrappers.  Include this header after any
 * other headers that may define these symbols.
 * @{ */
#define malloc(s) csilk_test_malloc(s)
#define calloc(n, s) csilk_test_calloc(n, s)
#define realloc(p, s) csilk_test_realloc(p, s)
/** @} */

#endif /* TEST_OOM */

/**
 * @name Context Test Helpers
 * Helpers to create and manipulate opaque csilk_ctx_t objects in unit tests.
 * @{ */

/** @brief Create a new mock context for testing (heap-allocated).
 *  @return Pointer to new context, or nullptr on failure. */
csilk_ctx_t* csilk_test_ctx_new(void);

/** @brief Free a mock context created via csilk_test_ctx_new().
 *  @param c  The context to free. */
void csilk_test_ctx_free(csilk_ctx_t* c);

/** @brief Set the handler chain for a test context.
 *  @param c        The request context.
 *  @param handlers nullptr-terminated array of handler functions. */
void csilk_test_ctx_set_handlers(csilk_ctx_t* c, csilk_handler_t* handlers);

/** @brief Manually set the request method and path for testing.
 *  @param c       The request context.
 *  @param method  HTTP method string (e.g. "GET"). Not copied.
 *  @param path    Decoded URL path string. Not copied. */
void csilk_test_ctx_set_request(csilk_ctx_t* c, const char* method, const char* path);

/** @brief Manually set metadata for the current handler (mocking matched route).
 *  @param c              The request context.
 *  @param perm_required  Permission string. Not copied.
 *  @param perm_resource  Resource pattern. Not copied. */
void csilk_test_ctx_set_handler_metadata(csilk_ctx_t* c,
                                         const char*  perm_required,
                                         const char*  perm_resource);

/** @brief Manually set the request body for testing.
 *  @param c    The request context.
 *  @param body The raw request body string. Not copied.
 *  @param len  Length of the body. */
void csilk_test_ctx_set_body(csilk_ctx_t* c, const char* body, size_t len);

/** @brief Manually add a path parameter for testing.
 *  @param c     The request context.
 *  @param key   Parameter name. Copied.
 *  @param value Parameter value. Copied. */
void csilk_test_ctx_add_param(csilk_ctx_t* c, const char* key, const char* value);

/** @brief Count response headers with a given key and optional value substring.
 *  @param c               The request context.
 *  @param key             Header key to look up.
 *  @param value_contains  Optional substring to match in the value.
 *  @return Number of matching headers. */
int
csilk_test_ctx_count_response_headers(csilk_ctx_t* c, const char* key, const char* value_contains);

/** @} */

#endif /* CSILK_TEST_H */
