#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/json/json.h"

static void test_json_accessor_guard_branches(void);

int
main(void)
{
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "str_int", "42");
    csilk_json_add_string(obj, "str_double", "3.14159");
    csilk_json_add_string(obj, "str_bool_t", "true");
    csilk_json_add_string(obj, "str_bool_f", "false");
    csilk_json_add_string(obj, "str_dirty", "123abc");
    csilk_json_add_number(obj, "num_val", 100);
    csilk_json_add_bool(obj, "bool_val", true);

    /* 验证数字转换 */
    assert(csilk_json_as_int64(csilk_json_get(obj, "str_int"), 0) == 42);
    assert(csilk_json_as_int64(csilk_json_get(obj, "num_val"), 0) == 100);
    assert(csilk_json_as_int64(csilk_json_get(obj, "str_dirty"), -1) == -1); /* 脏数据安全回退 */
    assert(csilk_json_as_int64(csilk_json_get(obj, "non_exist"), -99) == -99);

    /* 验证浮点转换 */
    assert(csilk_json_as_double(csilk_json_get(obj, "str_double"), 0.0) > 3.14);
    assert(csilk_json_as_double(csilk_json_get(obj, "str_dirty"), -1.0) == -1.0);

    /* 验证布尔转换 */
    assert(csilk_json_as_bool(csilk_json_get(obj, "str_bool_t"), false) == true);
    assert(csilk_json_as_bool(csilk_json_get(obj, "str_bool_f"), true) == false);
    assert(csilk_json_as_bool(csilk_json_get(obj, "bool_val"), false) == true);

    csilk_json_free(obj);
    test_json_accessor_guard_branches();
    printf("test_json_as passed!\n");
    return 0;
}

/* Exercise the NULL-arg and type-mismatch guard branches across all the
 * pointer + value accessors, in both the immutable (parsed) and mutable
 * (csilk_json_object) representations. */
static void
test_json_accessor_guard_branches(void)
{
    /* ---- immutable (parsed) tree ---- */
    csilk_json_t* root = csilk_json_parse(
        "{\"s\":\"hi\",\"n\":12.5,\"i\":7,\"b\":true,\"o\":{},\"a\":[1,2],\"nul\":null}");
    assert(root != NULL);

    /* pointer accessors: NULL obj / NULL key / type mismatch */
    assert(csilk_json_get(NULL, "s") == NULL);
    assert(csilk_json_get(root, NULL) == NULL);
    assert(csilk_json_get_string(root, "n") == NULL); /* number, not string */
    assert(csilk_json_get_string(root, "s") != NULL);
    assert(csilk_json_get_object(root, "s") == NULL); /* not an object */
    assert(csilk_json_get_object(root, "o") != NULL);
    assert(csilk_json_get_array(root, "s") == NULL);
    assert(csilk_json_get_array(root, "a") != NULL);
    assert(csilk_json_get_int(root, "s") == 0);  /* string -> 0 */
    assert(csilk_json_get_int(root, "i") == 7);
    assert(csilk_json_get_int(root, "n") == 12); /* number -> int truncation */
    assert(csilk_json_get_bool(root, "s") == false);
    assert(csilk_json_get_bool(root, "b") == true);
    assert(csilk_json_get_number(root, "s") == 0.0);
    assert(csilk_json_get_number(root, "n") > 12.0);

    /* _v variants on NULL */
    assert(csilk_json_get_v(NULL, "x") == NULL);
    assert(csilk_json_get_string_v(NULL, "x") == NULL);
    assert(csilk_json_get_int_v(NULL, "x") == 0);
    assert(csilk_json_get_bool_v(NULL, "x") == false);
    assert(csilk_json_get_number_v(NULL, "x") == 0.0);

    /* array_get guards: NULL array, out-of-range, non-array */
    assert(csilk_json_array_get(NULL, 0) == NULL);
    csilk_json_t* arr = csilk_json_get_array(root, "a");
    assert(csilk_json_array_get(arr, 2) == NULL); /* out of range (size 2) */
    assert(csilk_json_array_get(arr, 0) != NULL); /* in range -> valid element */
    /* Passing a non-array (object) to array_get yields the invalid sentinel (NULL). */
    assert(csilk_json_array_get(root, 0) == NULL);

    /* value extractors on non-matching / NULL */
    csilk_json_t* nval = csilk_json_get(root, "n");
    assert(csilk_json_string_value(nval) == NULL); /* number not string */
    assert(csilk_json_int_value(nval) == 12);
    assert(csilk_json_uint_value(nval) == 12);
    csilk_json_t* nul = csilk_json_get(root, "nul");
    assert(csilk_json_is_null(nul));
    assert(csilk_json_string_value(nul) == NULL);

    csilk_json_free(root);

    /* ---- mutable (csilk_json_object) tree: same guards via the _v path ---- */
    csilk_json_t* m = csilk_json_object();
    csilk_json_add_string(m, "s", "hi");
    csilk_json_add_number(m, "n", 3.5);
    assert(csilk_json_get_string_v(m, "n") == NULL);
    assert(csilk_json_get_string_v(m, "s") != NULL);
    assert(csilk_json_get_object_v(m, "s") == NULL);
    assert(csilk_json_get_array_v(m, "s") == NULL);
    assert(csilk_json_get_int_v(m, "s") == 0);
    assert(csilk_json_get_int_v(m, "n") == 3);
    assert(csilk_json_get_bool_v(m, "n") == false);
    assert(csilk_json_array_get_v(NULL, 0) == NULL);

    /* value extractors on a mutable number node */
    csilk_json_t* mn = csilk_json_get(m, "n");
    assert(csilk_json_string_value(mn) == NULL);
    assert(csilk_json_number_value(mn) > 3.0);
    assert(csilk_json_int_value(mn) == 3);

    csilk_json_free(m);
}
