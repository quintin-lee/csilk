#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"

// Fuzz test for URL decoding logic

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }

    // Create a null-terminated string copy.  csilk_url_decode() can expand
    // each byte of input (invalid '%' → "%25"), so the output can reach 3x
    // the input length; size the buffer to that worst case plus NUL.
    char* input = malloc(size * 3 + 1);
    if (!input) {
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';

    // csilk_url_decode performs in-place decoding
    csilk_url_decode(input, size * 3 + 1);

    free(input);
    return 0;
}
