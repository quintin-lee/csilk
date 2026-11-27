#pragma once
/**
 * @file csilk.h
 * @brief Umbrella header for the csilk web framework.
 *
 * Includes all Web Core public module headers and conditionally includes
 * enabled optional domain module headers.  For finer-grained compilation,
 * include individual module headers directly.
 *
 * @version 1.3.0
 * @copyright MIT License
 */

#include "csilk/version.h"
#include "csilk/core/config/errors.h"
#include "csilk/core/types.h"
#include "csilk/core/json/json.h"
#include "csilk/core/config/hooks.h"
#include "csilk/core/config/config.h"
#include "csilk/crypto/crypto.h"
#include "csilk/core/ctx/context.h"
#include "csilk/core/ctx/async.h"
#include "csilk/core/primitives/codec.h"
#include "csilk/core/ctx/response.h"
#include "csilk/core/router.h"
#include "csilk/core/group.h"
#include "csilk/core/middleware.h"
#include "csilk/core/session_store.h"
#include "csilk/protocols/websocket.h"
#include "csilk/protocols/sse.h"
#include "csilk/core/server/server.h"
#include "csilk/core/http/h2.h"
#include "csilk/core/server/hot_reload.h"
#include "csilk/core/security.h"
#include "csilk/core/plugin.h"
#include "csilk/core/service_locator.h"
#include "csilk/app/app.h"
#ifdef CSILK_HAS_MQ
#include "csilk/messaging/mq.h"
#endif
#ifdef CSILK_HAS_WORKFLOW
#include "csilk/app/workflow.h"
#endif
#ifdef CSILK_HAS_OBSERVABILITY
#include "csilk/app/admin.h"
#include "csilk/util/flamegraph.h"
#include "csilk/middleware/otlp_trace.h"
#endif
