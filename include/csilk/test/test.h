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
 *   // Run code under test — the 5th malloc/calloc/realloc returns NULL.
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
 * When >= 0, allocations at or beyond this count return NULL.
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
 * @return Pointer to allocated memory, or NULL if the failure threshold
 *         has been reached.
 */
static inline void* csilk_test_malloc(size_t size) {
  if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) return NULL;
  g_oom_count++;
  return malloc(size);
}

/**
 * @brief Mock calloc that fails after g_oom_fail_after calls.
 *
 * @param nmemb  Number of elements.
 * @param size   Size of each element.
 * @return Pointer to zero-initialised allocated memory, or NULL if the
 *         failure threshold has been reached.
 */
static inline void* csilk_test_calloc(size_t nmemb, size_t size) {
  if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) return NULL;
  g_oom_count++;
  return calloc(nmemb, size);
}

/**
 * @brief Mock realloc that fails after g_oom_fail_after calls.
 *
 * @param ptr   Previously allocated pointer (may be NULL).
 * @param size  New size in bytes.
 * @return Pointer to resized memory, or NULL if the failure threshold
 *         has been reached.
 */
static inline void* csilk_test_realloc(void* ptr, size_t size) {
  if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) return NULL;
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

#endif /* CSILK_TEST_H */
