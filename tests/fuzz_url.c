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

	// Create a null-terminated string copy
	char* input = malloc(size + 1);
	if (!input) {
		return 0;
	}
	memcpy(input, data, size);
	input[size] = '\0';

	// csilk_url_decode performs in-place decoding
	csilk_url_decode(input);

	free(input);
	return 0;
}
