/**
 * @file h2.c
 * @brief HTTP/2 module - thin wrapper for backward compatibility.
 *
 * Implementation split across:
 *   h2_callbacks.c  - nghttp2 callbacks
 *   h2_session.c    - session/stream management
 *   h2_response.c   - response sending and push
 */

#include "csilk/http/h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
