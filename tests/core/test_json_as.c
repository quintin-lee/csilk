#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/json.h"

int main(void) {
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
    printf("test_json_as passed!\n");
    return 0;
}
