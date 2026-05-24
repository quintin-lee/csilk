#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_internal.h"

/** @brief Find a character in a string (helper). */
static const char* str_find(const char* s, char c) {
  while (s && *s) {
    if (*s == c) return s;
    s++;
  }
  return NULL;
}

/** @brief Check if a string looks like a valid email address. */
static int is_valid_email(const char* s) {
  if (!s || !*s) return 0;
  const char* at = str_find(s, '@');
  if (!at || at == s || at[1] == '\0') return 0;
  const char* dot = str_find(at + 1, '.');
  if (!dot || dot == at + 1 || dot[1] == '\0') return 0;
  if (dot - at < 2) return 0;
  return 1;
}

/** @brief Run all validation rules and return first error, or NULL on success.
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
