/**
 * @file http1_response.c
 * @brief HTTP/1.1 response module - thin wrapper for backward compatibility.
 *
 * Implementation split across:
 *   http1_serialize.c - response serialization
 *   http1_write.c     - write pipeline
 *   http1_pipeline.c  - post-response cleanup
 */

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
