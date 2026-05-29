/**
 * @file server_internal.h
 * @brief Internal layout of csilk_server_s and csilk_client_s.
 *
 * This header defines the memory layout of the server and client structures.
 * It is included by src/core/server.c and other internal framework code
 * that needs access to the server's internal state (e.g., MQ, loop, hooks).
 *
 * @copyright MIT License
 */

#ifndef CSILK_SERVER_INTERNAL_H
#define CSILK_SERVER_INTERNAL_H

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <uv.h>
#include <llhttp.h>

#include "csilk/csilk.h"

/** @brief Default idle timeout in milliseconds. */
#define CSILK_DEFAULT_IDLE_TIMEOUT 5000
/** @brief Default maximum request body size in bytes. */
#define CSILK_DEFAULT_MAX_BODY_SIZE (1024UL * 1024UL)
/** @brief Default maximum request header size in bytes. */
#define CSILK_DEFAULT_MAX_HEADER_SIZE (64UL * 1024UL)
/** @brief Default TCP listen backlog. */
#define CSILK_DEFAULT_LISTEN_BACKLOG 128
/** @brief Default request arena chunk size. */
#define CSILK_DEFAULT_ARENA_SIZE 4096

/** @brief Hook handler node in a linked list. */
typedef struct csilk_hook_node_s {
	void* handler;
	struct csilk_hook_node_s* next;
} csilk_hook_node_t;

/** @brief Forward declaration for client connection structure. */
typedef struct csilk_client_s csilk_client_t;

/**
 * @brief Main Server structure — represents the core HTTP server instance.
 *
 * Manages the libuv event loop, HTTP listener, configuration, global
 * middleware chain, hook registrations, and client connection pooling.
 * Thread-safe for multi-threaded operation via atomic counters and mutexes.
 */
struct csilk_server_s {
	uv_loop_t* loop;		 /**< libuv event loop. */
	csilk_router_t* router;		 /**< Associated router instance. */
	uv_tcp_t server_handle;		 /**< TCP server handle. */
	uv_signal_t sig_handle;		 /**< SIGINT signal handler. */
	uv_async_t async_handle;	 /**< Async handle for cross-thread wakeup. */
	llhttp_settings_t settings;	 /**< HTTP parser callback settings. */
	csilk_server_config_t config;	 /**< Server configuration. */
	csilk_handler_t middlewares[32]; /**< Global middlewares. */
	int middleware_count;		 /**< Number of global middlewares. */
	int max_connections;		 /**< Max concurrent connections (0=unlimited). */
	atomic_int active_connections;	 /**< Current connection count (atomic). */
	/* close tracking for async shutdown — see csilk_server_free */
	uv_thread_t* worker_tids;		/**< Worker thread IDs (NULL if single-thread). */
	int worker_count;			/**< Number of worker threads created. */
	uv_async_t* worker_stop_async;		/**< Per-worker async handles for graceful stop. */
	int worker_stop_count;			/**< Number of worker_stop_async entries. */
	csilk_handler_t not_found_handler;	/**< Custom 404 handler (NULL = default). */
	char* spa_doc_root;			/**< SPA fallback doc root (NULL = disabled). */
	csilk_storage_driver_t* storage_driver; /**< Context storage driver. */
	csilk_crypto_driver_t* crypto_driver;	/**< Crypto algorithm driver. */
	csilk_cipher_driver_t* cipher_driver;	/**< Cipher algorithm driver. */
	SSL_CTX* ssl_ctx;			/**< OpenSSL context. */
	csilk_mq_t* mq;				/**< Message Queue instance. */
	csilk_hook_node_t* hooks[CSILK_HOOK_COUNT]; /**< Registered hooks. */
	csilk_client_t* active_clients;		    /**< Head of active connections list. */
	uv_mutex_t clients_mutex;		    /**< Mutex for active clients list. */
	csilk_client_t* client_pool[32];	    /**< Connection object free list. */
	int client_pool_count;			    /**< Number of free clients in pool. */
	uv_mutex_t pool_mutex;			    /**< Mutex for connection pool access. */
};

/** @brief Client connection structure — represents a single TCP connection.
 *
 * Holds the libuv stream handle, HTTP parser state, timers for keep-alive
 * and timeouts, TLS context (if HTTPS), and the request/response context.
 * Clients are pooled and reused for performance.
 */
struct csilk_client_s {
	uv_tcp_t handle;	      /**< libuv TCP stream handle. */
	uv_timer_t timer;	      /**< Connection idle (keep-alive) timer. */
	uv_timer_t read_timer;	      /**< Read timeout timer. */
	uv_timer_t write_timer;	      /**< Write timeout timer. */
	uv_timer_t request_timer;     /**< Request timeout timer. */
	int close_pending;	      /**< Pending close refs before freeing client. */
	llhttp_t parser;	      /**< HTTP request parser. */
	csilk_server_t* server;	      /**< Owning server instance. */
	csilk_ctx_t ctx;	      /**< Request context for this connection. */
	size_t total_header_size;     /**< Total size of headers parsed so far. */
	size_t header_count;	      /**< Number of headers parsed so far. */
	size_t current_url_capacity;  /**< Allocated size of current_url. */
	size_t header_field_capacity; /**< Allocated size of current_header_field. */
	size_t header_value_capacity; /**< Allocated size of current_header_value. */
	char* current_url;	      /**< Current URL being parsed. */
	char* current_header_field;   /**< Temporary header field name. */
	char* current_header_value;   /**< Temporary header field value. */
	SSL* ssl;		      /**< OpenSSL session object. */
	BIO* read_bio;		      /**< BIO for reading encrypted data. */
	BIO* write_bio;		      /**< BIO for writing encrypted data. */
	struct csilk_client_s* next;  /**< Next client in active list. */
	struct csilk_client_s* prev;  /**< Previous client in active list. */
};

#endif /* CSILK_SERVER_INTERNAL_H */
