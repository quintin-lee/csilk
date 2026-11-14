#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "drivers/db/db_internal.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 65536) {
        return 0;
    }

    char* sql = malloc(size + 1);
    if (!sql) {
        return 0;
    }
    memcpy(sql, data, size);
    sql[size] = '\0';

    int   param_count = 0;
    char* rewritten = csilk_sql_rewrite_placeholders(sql, &param_count);
    free(rewritten);
    free(sql);
    return 0;
}
