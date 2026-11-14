#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "csilk.h"

// Fuzz test for YAML configuration loader

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }

    csilk_config_t config;
    if (csilk_load_config_memory(data, size, &config) == 0) {
        const char* error_msg = nullptr;
        csilk_config_validate(&config, &error_msg);
        csilk_config_free(&config);
    } else {
        csilk_config_free(&config);
    }

    return 0;
}
