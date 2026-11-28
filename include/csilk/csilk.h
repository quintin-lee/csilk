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
#ifdef CSILK_HAS_DB
#include "csilk/drivers/db.h"
#endif
#ifdef CSILK_HAS_AI
#include "csilk/drivers/ai.h"
#include "csilk/drivers/ai/tool_registry.h"
#endif
#ifdef CSILK_HAS_VECTOR
#include "csilk/drivers/vector.h"
#endif
#ifdef CSILK_HAS_MQ
#include "csilk/messaging/mq.h"
#endif
#ifdef CSILK_HAS_MCP
#include "csilk/protocols/mcp.h"
#endif
#ifdef CSILK_HAS_GRPC
#include "csilk/protocols/grpc.h"
#endif
#ifdef CSILK_HAS_HTTP3
#include "csilk/protocols/h3.h"
#endif
#ifdef CSILK_HAS_PERM
#include "csilk/permission/perm.h"
#endif
#ifdef CSILK_HAS_WASM
#include "csilk/wasm/wasm.h"
#include "csilk/wasm/plugin.h"
#endif
#ifdef CSILK_HAS_CSV
#include "csilk/util/csv.h"
#endif
#ifdef CSILK_HAS_WORKFLOW
#include "csilk/workflow/workflow.h"
#endif
#ifdef CSILK_HAS_OBSERVABILITY
#include "csilk/observability/admin.h"
#include "csilk/observability/flamegraph.h"
#include "csilk/observability/otlp_trace.h"
#endif
