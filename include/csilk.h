/**
 * @file csilk.h
 * @brief Umbrella header for the csilk web framework.
 *
 * Includes all public module headers.  Include this single header to
 * access the complete csilk API.  For finer-grained compilation, include
 * individual module headers (csilk/types.h, csilk/router.h, etc.).
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_H
#define CSILK_H

#include "csilk/version.h"
#include "csilk/errors.h"
#include "csilk/types.h"
#include "csilk/hooks.h"
#include "csilk/config.h"
#include "csilk/crypto.h"
#include "csilk/context.h"
#include "csilk/core/codec.h"
#include "csilk/response.h"
#include "csilk/router.h"
#include "csilk/group.h"
#include "csilk/middleware.h"
#include "csilk/websocket.h"
#include "csilk/sse.h"
#include "csilk/mq.h"
#include "csilk/server.h"
#include "csilk/workflow.h"
#include "csilk/admin.h"

#endif /* CSILK_H */
