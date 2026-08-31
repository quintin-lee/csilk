#pragma once
/**
 * @file csilk/core/primitives/base.h
 * @brief Foundation primitives header for the csilk framework.
 *
 * Exposes core data types, errors, bump arena allocator, bounded buffers,
 * key-value store, string views, and atomic sync primitives without pulling
 * in JSON, YAML, OpenSSL, or network I/O dependencies.
 *
 * @copyright MIT License
 */

#include "csilk/core/config/errors.h"
#include "csilk/core/types.h"
#include "csilk/core/primitives/bounded_buf.h"
#include "csilk/core/sync.h"
#include "csilk/version.h"
