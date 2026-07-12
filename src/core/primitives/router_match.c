/**
 * @file router_match.c
 * @brief Router SIMD-accelerated path segment extraction and trie matching.
 *
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "router_internal.h"

#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"), no_sanitize("address"))) static inline const char*
get_next_segment_avx512(const char** p, size_t* len)
{
    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return nullptr;
    }

    const char* start = *p;
    const char* curr = *p;

    __m512i slash_vec = _mm512_set1_epi8('/');
    __m512i zero_vec = _mm512_setzero_si512();

    while (1) {
        uintptr_t addr = (uintptr_t)curr;
        if ((addr & 4095) <= 4096 - 64) {
            __m512i   data = _mm512_loadu_si512((const __m512i*)curr);
            __mmask64 cmp_slash = _mm512_cmpeq_epi8_mask(data, slash_vec);
            __mmask64 cmp_zero = _mm512_cmpeq_epi8_mask(data, zero_vec);
            __mmask64 cmp_combined = cmp_slash | cmp_zero;
            if (cmp_combined != 0) {
                int idx = __builtin_ctzll(cmp_combined);
                curr += idx;
                break;
            }
            curr += 64;
        } else {
            if (*curr == '/' || *curr == '\0') {
                break;
            }
            curr++;
        }
    }

    *p = curr;
    *len = (size_t)(curr - start);
    return start;
}
#endif

#if defined(__x86_64__)
__attribute__((target("avx2"), no_sanitize("address"))) static inline const char*
get_next_segment_avx2(const char** p, size_t* len)
{
    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return nullptr;
    }

    const char* start = *p;
    const char* curr = *p;

    __m256i slash_vec = _mm256_set1_epi8('/');
    __m256i zero_vec = _mm256_setzero_si256();

    while (1) {
        uintptr_t addr = (uintptr_t)curr;
        if ((addr & 4095) <= 4096 - 32) {
            __m256i data = _mm256_loadu_si256((const __m256i*)curr);
            __m256i cmp_slash = _mm256_cmpeq_epi8(data, slash_vec);
            __m256i cmp_zero = _mm256_cmpeq_epi8(data, zero_vec);
            __m256i cmp_combined = _mm256_or_si256(cmp_slash, cmp_zero);
            int     mask = _mm256_movemask_epi8(cmp_combined);
            if (mask != 0) {
                int idx = __builtin_ctz(mask);
                curr += idx;
                break;
            }
            curr += 32;
        } else {
            if (*curr == '/' || *curr == '\0') {
                break;
            }
            curr++;
        }
    }

    *p = curr;
    *len = (size_t)(curr - start);
    return start;
}
#endif

#if defined(__ARM_NEON)
__attribute__((no_sanitize("address"))) static inline const char*
get_next_segment_neon(const char** p, size_t* len)
{
    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return nullptr;
    }

    const char* start = *p;
    const char* curr = *p;

    uint8x16_t slash_vec = vdupq_n_u8('/');
    uint8x16_t zero_vec = vdupq_n_u8('\0');

    while (1) {
        uintptr_t addr = (uintptr_t)curr;
        if ((addr & 4095) <= 4096 - 16) {
            uint8x16_t data = vld1q_u8((const uint8_t*)curr);
            uint8x16_t cmp_slash = vceqq_u8(data, slash_vec);
            uint8x16_t cmp_zero = vceqq_u8(data, zero_vec);
            uint8x16_t cmp_combined = vorrq_u8(cmp_slash, cmp_zero);

            uint64_t mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp_combined), 0);
            uint64_t mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp_combined), 1);

            if (mask_low != 0 || mask_high != 0) {
                if (mask_low != 0) {
                    int idx = __builtin_ctzll(mask_low) / 8;
                    curr += idx;
                } else {
                    int idx = __builtin_ctzll(mask_high) / 8;
                    curr += 8 + idx;
                }
                break;
            }
            curr += 16;
        } else {
            if (*curr == '/' || *curr == '\0') {
                break;
            }
            curr++;
        }
    }

    *p = curr;
    *len = (size_t)(curr - start);
    return start;
}
#endif

const char*
get_next_segment(const char** p, size_t* len)
{
#if defined(CSILK_HAS_AVX512)
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        return get_next_segment_avx512(p, len);
    }
#endif
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        return get_next_segment_avx2(p, len);
    }
#elif defined(__ARM_NEON)
    return get_next_segment_neon(p, len);
#endif

    if (!*p || **p == '\0') {
        return nullptr;
    }

    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return nullptr;
    }

    const char* start = *p;
    while (**p != '/' && **p != '\0') {
        (*p)++;
    }

    *len = (size_t)(*p - start);
    return start;
}

#if defined(__x86_64__)
#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"))) static inline int
csilk_memcmp_avx512(const char* s1, const char* s2, size_t n)
{
    if (n >= 64) {
        __m512i   v1 = _mm512_loadu_si512((const __m512i*)s1);
        __m512i   v2 = _mm512_loadu_si512((const __m512i*)s2);
        __mmask64 cmp = _mm512_cmpeq_epi8_mask(v1, v2);
        if (cmp != 0xFFFFFFFFFFFFFFFFULL) {
            return 0;
        }
        if (n == 64) {
            return 1;
        }
        return memcmp(s1 + 64, s2 + 64, n - 64) == 0;
    }
    return memcmp(s1, s2, n) == 0;
}
#endif

__attribute__((target("avx2"))) static inline int
csilk_memcmp_avx2(const char* s1, const char* s2, size_t n)
{
    if (n >= 32) {
        __m256i v1 = _mm256_loadu_si256((const __m256i*)s1);
        __m256i v2 = _mm256_loadu_si256((const __m256i*)s2);
        __m256i cmp = _mm256_cmpeq_epi8(v1, v2);
        int     mask = _mm256_movemask_epi8(cmp);
        if (mask != (int)0xFFFFFFFF) {
            return 0;
        }
        if (n == 32) {
            return 1;
        }
        return memcmp(s1 + 32, s2 + 32, n - 32) == 0;
    }
    return memcmp(s1, s2, n) == 0;
}
#endif

#if defined(__ARM_NEON)
static inline int
csilk_memcmp_neon(const char* s1, const char* s2, size_t n)
{
    if (n >= 16) {
        uint8x16_t v1 = vld1q_u8((const uint8_t*)s1);
        uint8x16_t v2 = vld1q_u8((const uint8_t*)s2);
        uint8x16_t cmp = vceqq_u8(v1, v2);
        uint64_t   mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t   mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (mask_low != UINT64_MAX || mask_high != UINT64_MAX) {
            return 0;
        }
        if (n == 16) {
            return 1;
        }
        return memcmp(s1 + 16, s2 + 16, n - 16) == 0;
    }
    return memcmp(s1, s2, n) == 0;
}
#endif

int
csilk_memcmp_fast(const char* s1, const char* s2, size_t n)
{
#if defined(CSILK_HAS_AVX512)
    static int has_avx512 = -1;
    if (has_avx512 < 0) {
        has_avx512 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
    }
    if (n >= 64 && has_avx512) {
        return csilk_memcmp_avx512(s1, s2, n);
    }
#endif
#if defined(__x86_64__)
    static int has_avx2 = -1;
    if (has_avx2 < 0) {
        has_avx2 = __builtin_cpu_supports("avx2");
    }
    if (n >= 32 && has_avx2) {
        return csilk_memcmp_avx2(s1, s2, n);
    }
#elif defined(__ARM_NEON)
    if (n >= 16) {
        return csilk_memcmp_neon(s1, s2, n);
    }
#endif

    if (n == 0) {
        return 1;
    }

#if defined(__x86_64__) || defined(__aarch64__)
    if (n >= 8) {
        if (*(const uint64_t*)s1 != *(const uint64_t*)s2) {
            return 0;
        }
        if (n == 8) {
            return 1;
        }
        s1 += 8;
        s2 += 8;
        n -= 8;
    }
    if (n >= 4) {
        if (*(const uint32_t*)s1 != *(const uint32_t*)s2) {
            return 0;
        }
        if (n == 4) {
            return 1;
        }
        s1 += 4;
        s2 += 4;
        n -= 4;
    }
    if (n >= 2) {
        if (*(const uint16_t*)s1 != *(const uint16_t*)s2) {
            return 0;
        }
        if (n == 2) {
            return 1;
        }
        s1 += 2;
        s2 += 2;
        n -= 2;
    }
    return *s1 == *s2;
#else
    return memcmp(s1, s2, n) == 0;
#endif
}

static csilk_handler_t*
try_match_static(csilk_router_node_t*     child,
                 const char*              method,
                 const char*              seg,
                 size_t                   len,
                 const char*              p,
                 csilk_ctx_t*             ctx,
                 csilk_method_handler_t** out_mh,
                 int                      use_simd)
{
    if (child->segment_len == len && child->segment[0] == seg[0]) {
        int match = 0;
        if (use_simd) {
            match = csilk_memcmp_fast(child->segment, seg, len);
        } else {
            match = (strncmp(child->segment, seg, len) == 0);
        }

        if (match) {
            CSILK_LOG_T("Router: STATIC child '%s' matches segment '%.*s', recursing",
                        child->segment,
                        (int)len,
                        seg);
            csilk_handler_t* r = match_node(child, method, p, ctx, out_mh);
            if (r) {
                return r;
            }
            CSILK_LOG_T("Router: backtrack - match failed deeper for STATIC child '%s'",
                        child->segment);
        }
    }
    return nullptr;
}

static csilk_handler_t*
try_match_param(csilk_router_node_t*     child,
                const char*              method,
                const char*              seg,
                size_t                   len,
                const char*              p,
                csilk_ctx_t*             ctx,
                csilk_method_handler_t** out_mh)
{
    int param_added = 0;
    if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        if (ctx->arena) {
            ctx->params[ctx->params_count].key = csilk_arena_strdup(ctx->arena, child->segment);
            ctx->params[ctx->params_count].value = csilk_arena_strndup(ctx->arena, seg, len);
        } else {
            ctx->params[ctx->params_count].key = strdup(child->segment);
            ctx->params[ctx->params_count].value = malloc(len + 1);
            if (ctx->params[ctx->params_count].value) {
                memcpy(ctx->params[ctx->params_count].value, seg, len);
                ctx->params[ctx->params_count].value[len] = '\0';
            }
        }

        if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
            ctx->params_count++;
            param_added = 1;
            CSILK_LOG_T("Router: PARAM child '%s' matched segment "
                        "'%.*s', captured parameter",
                        child->segment,
                        (int)len,
                        seg);
        } else {
            if (!ctx->arena) {
                free(ctx->params[ctx->params_count].key);
                free(ctx->params[ctx->params_count].value);
            }
            CSILK_LOG_E("Router: failed to allocate path parameter "
                        "memory for key '%s'",
                        child->segment);
        }
    } else if (ctx) {
        CSILK_LOG_E("Router: path parameter limit (%d) exceeded while "
                    "parsing key '%s'",
                    CSILK_MAX_PARAMS,
                    child->segment);
    }

    csilk_handler_t* r = match_node(child, method, p, ctx, out_mh);

    if (!r && param_added) {
        CSILK_LOG_T("Router: backtrack - match failed deeper for PARAM "
                    "child '%s', rolling back parameter",
                    child->segment);
        ctx->params_count--;
        if (!ctx->arena) {
            free(ctx->params[ctx->params_count].key);
            free(ctx->params[ctx->params_count].value);
        }
    }
    return r;
}

static csilk_handler_t*
try_match_wildcard(csilk_router_node_t*     child,
                   const char*              method,
                   const char*              path,
                   csilk_ctx_t*             ctx,
                   csilk_method_handler_t** out_mh)
{
    CSILK_LOG_T("Router: WILDCARD child '%s' matches remaining path '%s'", child->segment, path);
    if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        const char* val_start = path;
        while (*val_start == '/') {
            val_start++;
        }

        if (ctx->arena) {
            ctx->params[ctx->params_count].key = csilk_arena_strdup(ctx->arena, child->segment);
            ctx->params[ctx->params_count].value = csilk_arena_strdup(ctx->arena, val_start);
        } else {
            ctx->params[ctx->params_count].key = strdup(child->segment);
            ctx->params[ctx->params_count].value = strdup(val_start);
        }

        if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
            ctx->params_count++;
            CSILK_LOG_T(
                "Router: captured wildcard parameter '%s' = '%s'", child->segment, val_start);
        } else {
            if (!ctx->arena) {
                free(ctx->params[ctx->params_count].key);
                free(ctx->params[ctx->params_count].value);
            }
            CSILK_LOG_E("Router: failed to allocate wildcard path parameter "
                        "memory for key '%s'",
                        child->segment);
        }
    } else if (ctx) {
        CSILK_LOG_E("Router: path parameter limit (%d) exceeded while parsing "
                    "wildcard key '%s'",
                    CSILK_MAX_PARAMS,
                    child->segment);
    }

    csilk_method_handler_t* mh = child->handlers;
    while (mh) {
        if (strcmp(mh->method, method) == 0) {
            if (out_mh) {
                *out_mh = mh;
            }
            CSILK_LOG_T("Router: matched handler for method '%s' at "
                        "wildcard node '%s'",
                        method,
                        child->segment);
            return mh->handlers;
        }
        mh = mh->next;
    }
    return nullptr;
}

csilk_handler_t*
match_node(csilk_router_node_t*     node,
           const char*              method,
           const char*              path,
           csilk_ctx_t*             ctx,
           csilk_method_handler_t** out_mh)
{
    int use_simd = (ctx && ctx->server) ? ctx->server->config.enable_simd : 1;

    CSILK_LOG_T("Router: matching node '%s' (type: %d) with remaining path '%s'",
                node->segment[0] ? node->segment : "/",
                node->type,
                path ? path : "empty");

    if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
        CSILK_LOG_T("Router: reached leaf/terminal match at node '%s'",
                    node->segment[0] ? node->segment : "/");
        csilk_method_handler_t* mh = node->handlers;
        while (mh) {
            if (strcmp(mh->method, method) == 0) {
                if (out_mh) {
                    *out_mh = mh;
                }
                CSILK_LOG_T("Router: matched handler for method '%s' at node '%s'",
                            method,
                            node->segment[0] ? node->segment : "/");
                return mh->handlers;
            }
            mh = mh->next;
        }
        CSILK_LOG_T("Router: no handler for method '%s' at node '%s'",
                    method,
                    node->segment[0] ? node->segment : "/");
        return nullptr;
    }

    const char* p = path;
    size_t      len;
    const char* seg = get_next_segment(&p, &len);
    if (!seg) {
        CSILK_LOG_T("Router: no more segments found in path '%s'", path);
        return nullptr;
    }

    CSILK_LOG_T("Router: testing segment '%.*s' against %d children of node '%s'",
                (int)len,
                seg,
                node->children_count,
                node->segment[0] ? node->segment : "/");

    csilk_handler_t* result = nullptr;
    for (int i = 0; i < node->children_count; i++) {
        csilk_router_node_t* child = node->children[i];
        if (child->type == CSILK_NODE_STATIC) {
            result = try_match_static(child, method, seg, len, p, ctx, out_mh, use_simd);
        } else if (child->type == CSILK_NODE_PARAM) {
            result = try_match_param(child, method, seg, len, p, ctx, out_mh);
        } else if (child->type == CSILK_NODE_WILDCARD) {
            result = try_match_wildcard(child, method, path, ctx, out_mh);
        }
        if (result) {
            break;
        }
    }

    return result;
}
