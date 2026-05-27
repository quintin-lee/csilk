/**
 * @file validate.c
 * @brief Request parameter validation middleware (required, type checking,
 * email).
 * @copyright MIT License
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief Find the first occurrence of a character in a string (helper).
 *
 * @param s  The string to search (may be NULL).
 * @param c  The character to locate.
 *
 * @return Pointer to the first occurrence of c in s, or NULL if c is not
 *         found or s is NULL.
 */
static const char* str_find(const char* s, char c) {
  while (s && *s) {
    if (*s == c) return s;
    s++;
  }
  return NULL;
}

/**
 * @brief Check if a string looks like a valid email address.
 *
 * Performs a basic syntactic validation: the string must contain exactly one
 * '@' character, the local part and domain must be non-empty, and the domain
 * must contain at least one dot after the '@' with non-empty segments. No
 * whitespace characters are permitted anywhere in the address.
 *
 * @param s  The string to validate. Must be null-terminated.
 *
 * @return 1 if the string passes the basic email format check, 0 otherwise.
 *
 * @note This is NOT a full RFC 5322 validator. It does not check for
 *       quoted local parts, IP address literals, IDN, or special characters.
 *       Use a dedicated validation library for strict email validation.
 */
static int is_valid_email(const char* s) {
  if (!s || !*s) return 0;
  /* Basic email syntax check:
     1. Exactly one '@' character.
     2. No whitespace allowed (RFC 5321 §4.1.2).
     3. Local part and domain must both be non-empty.
     4. Domain must contain at least one dot with non-empty TLD segment.
     This is intentionally simple — does NOT validate quoted locals,
     IP literals, or special characters per RFC 5322. */
  int at_count = 0;
  const char* at_ptr = NULL;

  for (const char* p = s; *p; p++) {
    if (*p == '@') {
      at_count++;
      at_ptr = p;
    } else if (isspace((unsigned char)*p)) {
      return 0; /* No whitespace in email addresses. */
    }
  }

  /* Must have exactly one '@' and both sides must be non-empty. */
  if (at_count != 1 || at_ptr == s || at_ptr[1] == '\0') return 0;

  /* Domain must contain at least one '.' with non-empty TLD. */
  const char* dot = strrchr(at_ptr + 1, '.');
  if (!dot || dot == at_ptr + 1 || dot[1] == '\0') return 0;

  return 1;
}

/**
 * @brief Run all validation rules and return the first error, or NULL on
 *        success.
 *
 * Iterates through an array of validation rules, stopping at the first rule
 * that fails. Each rule specifies the source of the value (query, form,
 * header, cookie, or automatic fallback) and the validation flags to apply
 * (CSILK_VALID_REQUIRED, CSILK_VALID_INT, CSILK_VALID_STRING,
 * CSILK_VALID_EMAIL).
 *
 * For integer validation, an optional [min, max] range can be specified.
 * For string validation, an optional [min, max] length range can be
 * specified. Ranges are ignored when min >= max.
 *
 * @param c     The request context to extract values from.
 * @param rules A null-terminated array of csilk_valid_rule_t rules (the
 *              last entry must have field == NULL). Must not be NULL.
 *
 * @return NULL if all rules pass, or a pointer to the field name (string
 *         from the rule definition) of the first field that fails.
 *
 * @note When no explicit source is specified, the function first tries the
 *       query string, then falls back to the form body.
 * @warning The returned error pointer points into the rules array; it must
 *          not be freed or dereferenced after the rules array goes out of
 *          scope.
 */
const char* csilk_validate(csilk_ctx_t* c, const csilk_valid_rule_t* rules) {
  if (!c || !rules) return NULL;

  for (const csilk_valid_rule_t* r = rules; r->field; r++) {
    const char* value = NULL;

    if (r->source && strcmp(r->source, "query") == 0) {
      value = csilk_get_query(c, r->field);
    } else if (r->source && strcmp(r->source, "form") == 0) {
      value = csilk_get_form_field(c, r->field);
    } else if (r->source && strcmp(r->source, "header") == 0) {
      value = csilk_get_header(c, r->field);
    } else if (r->source && strcmp(r->source, "cookie") == 0) {
      value = csilk_get_cookie(c, r->field);
    } else {
      /* Default source: check query string first, fall back to form body.
         This allows e.g. GET ?name=value and POST form fields to be
         validated with the same rule. */
      value = csilk_get_query(c, r->field);
      if (!value) value = csilk_get_form_field(c, r->field);
    }

    if (!value && (r->flags & CSILK_VALID_REQUIRED)) {
      return r->field;
    }

    if (!value) continue;

    if (r->flags & CSILK_VALID_INT) {
      const char* p = value;
      if (*p == '-') p++;
      if (!*p) return r->field;
      while (*p) {
        if (!isdigit((unsigned char)*p)) return r->field;
        p++;
      }
      long num = atol(value);
      if (r->min < r->max) {
        if (num < (long)r->min || num > (long)r->max) return r->field;
      }
    }

    if (r->flags & CSILK_VALID_STRING) {
      size_t slen = strlen(value);
      if (r->min < r->max) {
        if ((int)slen < r->min || (int)slen > r->max) return r->field;
      }
    }

    if (r->flags & CSILK_VALID_EMAIL) {
      if (!is_valid_email(value)) return r->field;
    }
  }

  return NULL;
}
