/**
 * @file tls.c
 * @brief TLS/SSL handshake, BIO-pair encryption/decryption, and ALPN.
 *
 * Implements OpenSSL integration: SSL_CTX creation, per-client TLS session
 * setup, memory-BIO encrypted I/O, and ALPN protocol negotiation (HTTP/1.1
 * vs HTTP/2).
 * @copyright MIT License
 */

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <llhttp.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "core/srv_internal.h"
#include "h2.h"
#include "srv_impl.h"

/* --- ALPN Selection --- */

/** @brief OpenSSL ALPN selection callback.
 *
 * Negotiates the application-layer protocol during a TLS handshake.
 * Currently advertises both HTTP/2 ("h2") and HTTP/1.1 ("http/1.1")
 * and lets the client decide via SSL_select_next_proto().  The
 * advertised list order gives preference to h2 for future-proofing;
 * at present the server fully supports only HTTP/1.1.
 *
 * @param ssl    The SSL session (unused).
 * @param out    On success, set to point at the selected protocol string.
 * @param outlen On success, set to the length of the selected protocol.
 * @param in     Client-offered protocols (TLV format).
 * @param inlen  Length of the client-offered list.
 * @param arg    User-supplied argument (unused).
 * @return SSL_TLSEXT_ERR_OK on successful negotiation,
 *         SSL_TLSEXT_ERR_NOACK if no common protocol was found. */
static int
alpn_select_cb(SSL* ssl,
	       const unsigned char** out,
	       unsigned char* outlen,
	       const unsigned char* in,
	       unsigned int inlen,
	       void* arg)
{
	(void)ssl;
	(void)arg;

	int rv = SSL_select_next_proto((unsigned char**)out,
				       outlen,
				       (const unsigned char*)"\x02h2\x08http/1.1",
				       12,
				       in,
				       inlen);

	if (rv != OPENSSL_NPN_NEGOTIATED) {
		return SSL_TLSEXT_ERR_NOACK;
	}

	return SSL_TLSEXT_ERR_OK;
}

/* --- TLS Init / Cleanup --- */

/** @brief Initialize the server's TLS/SSL context using OpenSSL.
 *
 * Loads error strings, initializes SSL algorithms, creates a TLS server
 * method context, loads the certificate chain and private key from the
 * configured file paths, optionally loads a CA file, and optionally enables
 * peer verification. On any failure, the SSL context is freed and set to
 * nullptr (TLS is effectively disabled).
 *
 * @param s The server instance (config must have tls_cert_file and
 *          tls_key_file set if enable_tls is true). */
void
init_tls(csilk_server_t* s)
{
	SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();

	const SSL_METHOD* method = TLS_server_method();
	s->ssl_ctx = SSL_CTX_new(method);
	if (!s->ssl_ctx) {
		ERR_print_errors_fp(stderr);
		return;
	}

	SSL_CTX_set_alpn_select_cb(s->ssl_ctx, alpn_select_cb, nullptr);

	if (s->config.tls_cert_file && s->config.tls_key_file) {
		if (SSL_CTX_use_certificate_chain_file(s->ssl_ctx, s->config.tls_cert_file) <= 0) {
			ERR_print_errors_fp(stderr);
			goto error;
		}
		if (SSL_CTX_use_PrivateKey_file(
			s->ssl_ctx, s->config.tls_key_file, SSL_FILETYPE_PEM) <= 0) {
			ERR_print_errors_fp(stderr);
			goto error;
		}
	} else {
		CSILK_LOG_E("TLS enabled but cert/key files missing");
		goto error;
	}

	if (s->config.tls_ca_file) {
		if (SSL_CTX_load_verify_locations(s->ssl_ctx, s->config.tls_ca_file, nullptr) <=
		    0) {
			ERR_print_errors_fp(stderr);
		}
	}

	if (s->config.tls_verify_peer) {
		SSL_CTX_set_verify(
		    s->ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
	}

	return;

error:
	SSL_CTX_free(s->ssl_ctx);
	s->ssl_ctx = nullptr;
}

/** @brief Clean up the server's TLS/SSL context and global SSL state.
 *
 * Frees the SSL_CTX and calls EVP_cleanup() for OpenSSL global cleanup.
 *
 * @param s The server instance (may have ssl_ctx == nullptr). */
void
cleanup_tls(csilk_server_t* s)
{
	if (s->ssl_ctx) {
		SSL_CTX_free(s->ssl_ctx);
		s->ssl_ctx = nullptr;
	}
	EVP_cleanup();
}

/* --- Per-client TLS --- */

/** @brief Set up TLS for an individual client connection.
 *
 * Creates a new SSL session from the server's SSL_CTX, initializes memory
 * BIOs for reading and writing encrypted data, and starts the TLS handshake
 * by calling process_tls_read().
 *
 * @param client The client connection to set up TLS on.
 * @return 0 on success, -1 if SSL session creation or BIO setup fails. */
int
setup_client_tls(csilk_client_t* client)
{
	client->ssl = SSL_new(client->server->ssl_ctx);
	if (!client->ssl) {
		return -1;
	}

	client->read_bio = BIO_new(BIO_s_mem());
	client->write_bio = BIO_new(BIO_s_mem());
	if (!client->read_bio || !client->write_bio) {
		SSL_free(client->ssl);
		client->ssl = nullptr;
		return -1;
	}

	SSL_set_bio(client->ssl, client->read_bio, client->write_bio);
	SSL_set_accept_state(client->ssl);

	process_tls_read(client);
	return 0;
}

/** @brief Process incoming TLS data — complete the handshake or decrypt
 * application data.
 *
 * If the TLS handshake is not yet complete, performs SSL_do_handshake() and
 * flushes the write BIO. If the handshake is complete, calls SSL_read() in
 * a loop to decrypt application data and feeds the decrypted data to the
 * llhttp parser (or WebSocket frame parser).
 *
 * @param client The client connection with pending TLS data in the read BIO. */
void
process_tls_read(csilk_client_t* client)
{
	char buf[4096];
	int n;

	if (!SSL_is_init_finished(client->ssl)) {
		int r = SSL_do_handshake(client->ssl);
		flush_tls_write(client);
		if (r <= 0) {
			int err = SSL_get_error(client->ssl, r);
			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
				CSILK_LOG_E("TLS Handshake error: %d", err);
				uv_close((uv_handle_t*)&client->handle, on_close);
			}
			return;
		}
		const unsigned char* alpn_data;
		unsigned int alpn_len;
		SSL_get0_alpn_selected(client->ssl, &alpn_data, &alpn_len);
		if (alpn_data && alpn_len == 2 && strncmp((const char*)alpn_data, "h2", 2) == 0) {
			client->protocol = CSILK_PROTO_HTTP2;
			CSILK_LOG_D("ALPN negotiated HTTP/2");
			if (csilk_h2_init_session(client) != 0) {
				CSILK_LOG_E("Failed to initialize HTTP/2 session");
				uv_close((uv_handle_t*)&client->handle, on_close);
				return;
			}
		} else {
			client->protocol = CSILK_PROTO_HTTP1;
		}
	}

	while ((n = SSL_read(client->ssl, buf, sizeof(buf))) > 0) {
		if (client->ctx.is_websocket) {
			csilk_ws_parse_frame(&client->ctx, (const uint8_t*)buf, (size_t)n);
		} else if (client->protocol == CSILK_PROTO_HTTP2) {
			if (csilk_h2_process_data(client, (const uint8_t*)buf, (size_t)n) != 0) {
				CSILK_LOG_E("HTTP/2 processing error");
				if (!uv_is_closing((uv_handle_t*)&client->handle)) {
					uv_close((uv_handle_t*)&client->handle, on_close);
				}
				break;
			}
		} else {
			enum llhttp_errno err = llhttp_execute(&client->parser, buf, (size_t)n);
			if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
				if (err == HPE_CLOSED_CONNECTION) {
					llhttp_init(&client->parser,
						    HTTP_REQUEST,
						    &client->server->settings);
					client->parser.data = client;
				} else {
					CSILK_LOG_E("TLS Parse error: %s %s",
						    llhttp_errno_name(err),
						    client->parser.reason ? client->parser.reason
									  : "unknown reason");
					if (!uv_is_closing((uv_handle_t*)&client->handle)) {
						uv_close((uv_handle_t*)&client->handle, on_close);
					}
					break;
				}
			}
		}
	}

	if (n <= 0) {
		int err = SSL_get_error(client->ssl, n);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE &&
		    err != SSL_ERROR_ZERO_RETURN) {
			CSILK_LOG_E("TLS Read error: %d", err);
			if (!uv_is_closing((uv_handle_t*)&client->handle)) {
				uv_close((uv_handle_t*)&client->handle, on_close);
			}
		}
	}

	flush_tls_write(client);
}

/** @brief Flush buffered TLS encrypted data to the client socket.
 *
 * Reads encrypted data from the write BIO and sends it via libuv write
 * requests. Must be called after SSL_write() or SSL_do_handshake() to
 * ensure the encrypted output is actually transmitted.
 *
 * @param client The client connection whose write BIO should be drained. */
void
flush_tls_write(csilk_client_t* client)
{
	char buf[4096];
	int n;

	while ((n = BIO_read(client->write_bio, buf, sizeof(buf))) > 0) {
		uv_write_t* req = malloc(sizeof(uv_write_t));
		if (!req) {
			break;
		}

		char* data = malloc((size_t)n);
		if (!data) {
			free(req);
			break;
		}
		memcpy(data, buf, (size_t)n);

		uv_buf_t uv_buf = uv_buf_init(data, (unsigned int)n);
		req->data = data;
		uv_write(req, (uv_stream_t*)&client->handle, &uv_buf, 1, on_write);
	}
}
