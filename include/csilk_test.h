/**
 * @file csilk_test.h
 * @brief Test helpers for OOM and mock allocations.
 * @copyright MIT License
 */

#ifndef CSILK_TEST_H
#define CSILK_TEST_H

#include <stdlib.h>

#ifdef TEST_OOM

extern int g_oom_fail_after;
extern int g_oom_count;

static inline void* csilk_test_malloc(size_t size) {
  if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) return NULL;
  g_oom_count++;
  return malloc(size);
}

static inline void* csilk_test_calloc(size_t nmemb, size_t size) {
  if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) return NULL;
  g_oom_count++;
  return calloc(nmemb, size);
}

static inline void* csilk_test_realloc(void* ptr, size_t size) {
  if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) return NULL;
  g_oom_count++;
  return realloc(ptr, size);
}

#define malloc(s) csilk_test_malloc(s)
#define calloc(n, s) csilk_test_calloc(n, s)
#define realloc(p, s) csilk_test_realloc(p, s)

#endif /* TEST_OOM */

#endif /* CSILK_TEST_H */
