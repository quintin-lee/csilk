#pragma once
/**
 * @file service_locator.h
 * @brief Cross-plugin service locator for csilk plugin architecture.
 *
 * Plugins can register named services (e.g. "ai_engine", "mq_bus",
 * "vector_index") that other plugins can discover at runtime without
 * direct linking or compile-time dependencies.
 *
 * @copyright MIT License
 */

#include <stddef.h>

#include "csilk/core/types.h"
#include "csilk/core/server/server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register a named service with the server.
 *
 * Plugins call this during init to publish their service handle.
 * Other plugins retrieve it with csilk_core_service_get().
 *
 * @param server  Server instance.
 * @param name    Service name (e.g. "ai_engine").
 * @param service Opaque service handle pointer.
 * @return 0 on success, -1 if name already registered. */
int csilk_core_service_register(csilk_server_t* server, const char* name, void* service);

/** @brief Look up a registered service by name.
 *
 * @param server Server instance.
 * @param name   Service name.
 * @return Service handle pointer, or NULL if not found. */
void* csilk_core_service_get(csilk_server_t* server, const char* name);

#ifdef __cplusplus
}
#endif
