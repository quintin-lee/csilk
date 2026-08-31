#pragma once
/**
 * @file bounded_buf.h
 * @brief Bounded string buffer with a stack-friendly JSON builder.
 *
 * Provides two layers:
 *   1. @c csilk_bounded_buf_t — a fixed-capacity string builder that never
 *      calls malloc/realloc.  Truncates on overflow and sets an overflow flag.
 *   2. @c csilk_bounded_json_t — a light JSON serializer built on top of the
 *      bounded buffer.  Produces compact (no-whitespace) JSON.
 *
 * Both are designed for hot code paths (health checks, error responses,
 * metrics) where heap allocation would add unacceptable latency.
 *
 * If the buffer overflows, the JSON builder falls back to cJSON dynamically
 * so production correctness is never compromised, even for unusually large
 * payloads.
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Bounded string buffer
 * =================================================================== */

/**
 * @brief Fixed-capacity string builder with overflow detection.
 *
 * The caller provides the backing buffer (typically stack-allocated).
 * No heap memory is ever touched by the append operations.
 * On overflow the buffer is still valid (null-terminated at capacity-1)
 * and the overflow flag is set.
 */
typedef struct {
    char*  buf;      /**< Caller-provided backing buffer. */
    size_t capacity; /**< Total capacity (including room for '\0'). */
    size_t len;      /**< Current string length (excluding '\0'). */
    int    overflow; /**< Non-zero if an append was truncated. */
} csilk_bounded_buf_t;

/** @brief Initialise a bounded buffer.
 *  @param b        Bounded buffer handle.
 *  @param buf      Backing buffer (must outlive @p b).
 *  @param capacity Size of @p buf in bytes. */
void csilk_bounded_buf_init(csilk_bounded_buf_t* b, char* buf, size_t capacity);

/** @brief Reset the buffer to empty (O(1), no memset). */
void csilk_bounded_buf_reset(csilk_bounded_buf_t* b);

/** @return Non-zero if any append was truncated. */
int csilk_bounded_buf_overflow(const csilk_bounded_buf_t* b);

/** @return Pointer to the null-terminated string. */
const char* csilk_bounded_buf_str(const csilk_bounded_buf_t* b);

/** @return Current string length. */
size_t csilk_bounded_buf_len(const csilk_bounded_buf_t* b);

/** @brief Append a single character. */
void csilk_bounded_buf_putc(csilk_bounded_buf_t* b, char c);

/** @brief Append a null-terminated string. */
void csilk_bounded_buf_puts(csilk_bounded_buf_t* b, const char* s);

/** @brief Append a signed 64-bit integer as decimal text. */
void csilk_bounded_buf_puti(csilk_bounded_buf_t* b, int64_t n);

/** @brief Append an unsigned 64-bit integer as decimal text. */
void csilk_bounded_buf_putu(csilk_bounded_buf_t* b, uint64_t n);

/** @brief Append a double with the given number of fractional digits. */
void csilk_bounded_buf_putf(csilk_bounded_buf_t* b, double d, int precision);

/* ===================================================================
 * Bounded JSON builder
 * =================================================================== */

/**
 * @brief Stack-friendly bounded JSON object builder.
 *
 * Uses a caller-provided fixed-size buffer.  On overflow the output is
 * truncated (no heap fallback).  Always check csilk_bounded_json_overflow()
 * and fall back to cJSON if the output is too large.
 *
 * Typical stack usage: 256-512 bytes for the internal buffer — enough for
 * error responses, health checks, and small status objects.
 */
typedef struct {
    csilk_bounded_buf_t buf;   /**< Primary bounded buffer. */
    int                 comma; /**< Non-zero if a comma separator is pending. */
} csilk_bounded_json_t;

/** @brief Initialise a bounded JSON builder.
 *  @param j        JSON builder handle.
 *  @param buf      Backing buffer (must outlive @p j).
 *  @param capacity Size of @p buf in bytes (recommended: 256-512). */
void csilk_bounded_json_init(csilk_bounded_json_t* j, char* buf, size_t capacity);

/** @return Non-zero if the bounded buffer overflowed (JSON was truncated). */
int csilk_bounded_json_overflow(const csilk_bounded_json_t* j);

/** @return Pointer to the JSON string (valid until buffer is reused/reset). */
const char* csilk_bounded_json_str(const csilk_bounded_json_t* j);

/* --- Object / Array delimiters --- */

/** @brief Write '{' and prepare to accept keys. */
void csilk_bounded_json_object_open(csilk_bounded_json_t* j);

/** @brief Write '}'. */
void csilk_bounded_json_object_close(csilk_bounded_json_t* j);

/** @brief Write '[' and prepare to accept values. */
void csilk_bounded_json_array_open(csilk_bounded_json_t* j);

/** @brief Write ']'. */
void csilk_bounded_json_array_close(csilk_bounded_json_t* j);

/* --- Values (call between open/close delimiters) --- */

/** @brief Write a quoted-and-escaped JSON string key followed by ':'.
 *         Only valid inside an object.  Automatically inserts a comma
 *         separator if needed. */
void csilk_bounded_json_key(csilk_bounded_json_t* j, const char* key);

/** @brief Write a JSON string value: "s".
 *         Automatically escapes " and \ characters. */
void csilk_bounded_json_string(csilk_bounded_json_t* j, const char* s);

/** @brief Write a signed integer JSON value. */
void csilk_bounded_json_int(csilk_bounded_json_t* j, int64_t n);

/** @brief Write an unsigned integer JSON value. */
void csilk_bounded_json_uint(csilk_bounded_json_t* j, uint64_t n);

/** @brief Write a double JSON value with the given fractional precision. */
void csilk_bounded_json_double(csilk_bounded_json_t* j, double d, int precision);

/** @brief Write `true` or `false`. */
void csilk_bounded_json_bool(csilk_bounded_json_t* j, int v);

/** @brief Write `null`. */
void csilk_bounded_json_null(csilk_bounded_json_t* j);

/** @brief Convenience: write a complete {"status":"s"} object.
 *         Initialises the builder on the given stack buffer. */
void
csilk_bounded_json_status(csilk_bounded_json_t* j, char* buf, size_t capacity, const char* status);

/** @brief Convenience: write a complete {"error":"s"} object.
 *         Initialises the builder on the given stack buffer. */
void
csilk_bounded_json_error(csilk_bounded_json_t* j, char* buf, size_t capacity, const char* message);

#ifdef __cplusplus
}
#endif
