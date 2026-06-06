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

	char filename[] = "/tmp/fuzz_yaml_XXXXXX";
	int fd = mkstemp(filename);
	if (fd == -1) {
		return 0;
	}

	if (write(fd, data, size) != (ssize_t)size) {
		close(fd);
		unlink(filename);
		return 0;
	}
	close(fd);

	csilk_config_t config;
	if (csilk_load_config(filename, &config) == 0) {
		const char* error_msg = nullptr;
		csilk_config_validate(&config, &error_msg);
		csilk_config_free(&config);
	}

	unlink(filename);
	return 0;
}
