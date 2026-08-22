/**
 * @file server_driver.c
 * @brief Pluggable driver injection — storage, crypto, cipher, QUIC.
 *
 * Provides the public setter API for injecting driver implementations
 * into a server instance. Each function is a simple NULL-gated pointer
 * assignment; no logic beyond validation.
 *
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "csilk/core/server.h"
#include "../internal/srv_internal.h"

/* --- Driver injection --- */

/** @brief Set the pluggable storage driver. */
void
csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver)
{
    if (server) {
        server->storage_driver = driver;
    }
}

/** @brief Set the pluggable cryptographic driver. */
void
csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver)
{
    if (server) {
        server->crypto_driver = driver;
    }
}

/** @brief Set the pluggable cipher driver. */
void
csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver)
{
    if (server) {
        server->cipher_driver = driver;
    }
}

/**
 * @brief Set the server's QUIC transport implementation.
 * @param[in] server    Server whose QUIC transport is set.
 * @param[in] transport QUIC transport handle (stored opaquely; may be NULL).
 * @note No-op if server is NULL. The transport is stored as an opaque pointer.
 */
void
csilk_server_set_quic_transport(csilk_server_t* server, csilk_quic_transport_t* transport)
{
    if (server) {
        server->quic_transport = (void*)transport;
    }
}
