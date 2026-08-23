/**
 * @file test_json_mutate.c
 * @brief Unit tests for yyjson mutable JSON mutation helpers (json_mutate.c).
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json.h"
#include "csilk/csilk.h"

static void
test_set_string_null_value(void)
{
    printf("Testing csilk_json_set_string with NULL value...\n");
    bool result = csilk_json_set_string(NULL, "hello");
    assert(result == false);
    printf("  passed\n");
}

static void
test_set_string_null_new_value(void)
{
    printf("Testing csilk_json_set_string with NULL new_value...\n");
    csilk_json_t* v = csilk_json_string_new("hello");
    assert(v != NULL);
    bool result = csilk_json_set_string(v, NULL);
    assert(result == false);
    csilk_json_free(v);
    printf("  passed\n");
}

static void
test_set_string_immutable_conversion(void)
{
    printf("Testing csilk_json_set_string immutable→mutable conversion...\n");
    csilk_json_t* v = csilk_json_string_new("old_value");
    assert(v != NULL);

    bool result = csilk_json_set_string(v, "new_value");
    assert(result == true);

    /* Verify the value changed */
    const char* final = csilk_json_string_value(v);
    assert(final != NULL);
    assert(strcmp(final, "new_value") == 0);

    csilk_json_free(v);
    printf("  passed\n");
}

static void
test_set_string_mutable_update(void)
{
    printf("Testing csilk_json_set_string on mutable value...\n");
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "key", "original");

    csilk_json_t* v = csilk_json_get(obj, "key");
    assert(v != NULL);

    bool result = csilk_json_set_string(v, "updated");
    assert(result == true);

    /* Verify update through the object */
    csilk_json_t* check = csilk_json_get(obj, "key");
    assert(check != NULL);
    assert(strcmp(csilk_json_string_value(check), "updated") == 0);

    csilk_json_free(obj);
    printf("  passed\n");
}

static void
test_set_string_non_string_value(void)
{
    printf("Testing csilk_json_set_string on non-string value...\n");
    csilk_json_t* v = csilk_json_number(42);
    assert(v != NULL);

    bool result = csilk_json_set_string(v, "should_fail");
    assert(result == false);

    /* Value should remain a number */
    double num = csilk_json_number_value(v);
    assert(num == 42.0);

    csilk_json_free(v);
    printf("  passed\n");
}

static void
test_set_string_empty_string(void)
{
    printf("Testing csilk_json_set_string with empty string...\n");
    csilk_json_t* v = csilk_json_string_new("hello");
    assert(v != NULL);

    bool result = csilk_json_set_string(v, "");
    assert(result == true);

    const char* final = csilk_json_string_value(v);
    assert(final != NULL);
    assert(strcmp(final, "") == 0);

    csilk_json_free(v);
    printf("  passed\n");
}

static void
test_set_string_immutable_no_idoc(void)
{
    printf("Testing csilk_json_set_string on immutable with no idoc...\n");
    csilk_json_t v = {0};
    v.u.raw = (void*)0x1234; /* fake pointer so !v->u.raw is false */
    v.doc.idoc = NULL;
    bool result = csilk_json_set_string(&v, "new");
    assert(result == false);
    printf("  passed\n");
}

static void
test_set_string_mutable_no_mdoc(void)
{
    printf("Testing csilk_json_set_string on mutable with no mdoc...\n");
    csilk_json_t v = {0};
    v.u.raw = (void*)0x1234;
    v.flags |= CSILK_JSON_F_MUTABLE;
    v.doc.mdoc = NULL;
    bool result = csilk_json_set_string(&v, "new");
    assert(result == false);
    printf("  passed\n");
}

int
main(void)
{
    test_set_string_null_value();
    test_set_string_null_new_value();
    test_set_string_immutable_conversion();
    test_set_string_mutable_update();
    test_set_string_non_string_value();
    test_set_string_empty_string();
    test_set_string_immutable_no_idoc();
    test_set_string_mutable_no_mdoc();

    printf("All test_json_mutate tests passed successfully!\n");
    return 0;
}
