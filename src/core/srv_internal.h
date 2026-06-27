/**
 * @file srv_types.h
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

/* Forward declarations for OpenSSL types — full definition from <openssl/ssl.h>
 * is only needed in tls.c, connection.c, and http1.c. */
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct bio_st BIO;

#include <stdatomic.h>
#include <uv.h>
#include <llhttp.h>
#include <nghttp2/nghttp2.h>

#include "csilk/csilk.h"
#include "core/ctx_internal.h"

/** @brief Default idle timeout in milliseconds. */
enum { CSILK_DEFAULT_IDLE_TIMEOUT = 5000 };
/** @brief Default maximum request body size in bytes. */
enum { CSILK_DEFAULT_MAX_BODY_SIZE = 1024UL * 1024UL };
/** @brief Default maximum request header size in bytes. */
enum { CSILK_DEFAULT_MAX_HEADER_SIZE = 64UL * 1024UL };
/** @brief Default maximum URL length in bytes. */
enum { CSILK_DEFAULT_MAX_URL_SIZE = 8192 };
/** @brief Default TCP listen backlog. */
enum { CSILK_DEFAULT_LISTEN_BACKLOG = 128 };
/** @brief Arena chunk size tiers for adaptive caching.
 *  Multiple tiers reduce large-request allocations while bounding memory overhead. */
enum { CSILK_ARENA_TIER_SMALL = 0 };  /* 4KB - standard requests */
enum { CSILK_ARENA_TIER_MEDIUM = 1 }; /* 16KB - large forms/JSON */
enum { CSILK_ARENA_TIER_LARGE = 2 };  /* 64KB - file uploads/large responses */
enum { CSILK_ARENA_TIER_COUNT = 3 };

enum { MAX_TLS_CHUNKS_PER_TIER = 8 }; /* Max cached chunks per tier */

/** @brief Default request arena chunk size. */
enum { CSILK_DEFAULT_ARENA_SIZE = 4096 };
/** @brief Client connection object pool capacity.
 *
 * Controls the maximum number of idle client objects cached for reuse.
 * When pool is exhausted, new clients are allocated from heap and returned
 * to pool on connection close. Increase for high-concurrency scenarios. */
enum { CSILK_CLIENT_POOL_SIZE = 32 };

/** @brief Hook handler node in a linked list. */
typedef struct csilk_hook_node_s {
	void* handler;
	struct csilk_hook_node_s* next;
} csilk_hook_node_t;

/** @brief Protocol type for a client connection. */
typedef enum { CSILK_PROTO_UNKNOWN, CSILK_PROTO_HTTP1, CSILK_PROTO_HTTP2 } csilk_protocol_t;

/** @brief Forward declaration for client connection structure. */
typedef struct csilk_client_s csilk_client_t;

/** @brief Task node for cross-thread dispatching. */
typedef struct csilk_dispatch_task_s {
	void (*cb)(void* arg);
	void* arg;
	struct csilk_dispatch_task_s* next;
} csilk_dispatch_task_t;

/**
 * @brief Per-worker connection pool and event-loop state.
 *
 * In multi-worker mode (SO_REUSEPORT), each worker thread gets its own pool.
 * This eliminates the global pool_mutex contention — pool_get and pool_put
 * are purely thread-local operations with zero locking overhead.  The main
 * event-loop thread uses pool index 0; worker threads use indices 1..N-1.
 *
 * For single-worker mode (worker_threads <= 1), only pool[0] is used and
 * the pool works identically to the old shared-pool model.
 */
typedef struct {
	csilk_server_t* server;				     /**< Owning server instance. */
	uv_loop_t loop;					     /**< This worker's libuv event loop. */
	uv_tcp_t server_handle;				     /**< Worker-local listen handle. */
	csilk_client_t* client_pool[CSILK_CLIENT_POOL_SIZE]; /**< Worker-local free list. */
	int client_pool_count;				     /**< Items in local free list. */
	uv_async_t stop_async;				     /**< Async for graceful worker stop. */
	int worker_index; /**< 0 = main loop, 1+ = worker threads. */
	csilk_arena_t* arena_pool[CSILK_CLIENT_POOL_SIZE]; /**< Pre-allocated arena pool. */
	int arena_pool_count;				   /**< Items in arena pool. */
	uv_async_t dispatch_async;	      /**< Cross-thread task dispatch async handle. */
	uv_mutex_t dispatch_mutex;	      /**< Mutex for dispatch queue. */
	csilk_dispatch_task_t* dispatch_head; /**< Head of dispatch queue. */
	csilk_dispatch_task_t* dispatch_tail; /**< Tail of dispatch queue. */
} worker_pool_t;

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
	uv_thread_t* worker_tids;	 /**< Worker thread IDs (nullptr if single-thread). */
	int worker_count;		 /**< Number of worker threads created. */
	worker_pool_t*
	    worker_pools; /**< Per-worker pools (size = worker_threads, index 0 = main loop). */
	int worker_pool_count;			/**< Number of worker pools (= worker_threads). */
	csilk_handler_t not_found_handler;	/**< Custom 404 handler (nullptr = default). */
	char* spa_doc_root;			/**< SPA fallback doc root (nullptr = disabled). */
	csilk_storage_driver_t* storage_driver; /**< Context storage driver. */
	csilk_crypto_driver_t* crypto_driver;	/**< Crypto algorithm driver. */
	csilk_cipher_driver_t* cipher_driver;	/**< Cipher algorithm driver. */
	SSL_CTX* ssl_ctx;			/**< OpenSSL context. */
	csilk_mq_t* mq;				/**< Message Queue instance. */
	csilk_hook_node_t* hooks[CSILK_HOOK_COUNT]; /**< Registered hooks. */
	csilk_client_t* active_clients;		    /**< Head of active connections list. */
	uv_mutex_t clients_mutex;		    /**< Mutex for active clients list. */
};

/** @brief Client connection structure — represents a single TCP connection.
 *
 * Holds the libuv stream handle, HTTP parser state, timers for keep-alive
 * and timeouts, TLS context (if HTTPS), and the request/response context.
 * Clients are pooled and reused for performance.
 */
struct csilk_client_s {
	uv_tcp_t handle;	  /**< libuv TCP stream handle. */
	uv_timer_t timer;	  /**< Connection idle (keep-alive) timer. */
	uv_timer_t read_timer;	  /**< Read timeout timer. */
	uv_timer_t write_timer;	  /**< Write timeout timer. */
	uv_timer_t request_timer; /**< Request timeout timer. */
	int close_pending;	  /**< Pending close refs before freeing client. */
	int async_ref;		  /**< Active asynchronous tasks reference counter. */

	csilk_protocol_t protocol;   /**< Protocol negotiated for this connection. */
	nghttp2_session* h2_session; /**< HTTP/2 session state (if HTTP/2). */
	csilk_ctx_t* h2_streams;     /**< Linked list of active HTTP/2 stream contexts. */

	llhttp_t parser; /**< HTTP request parser (if HTTP/1.1). */

	csilk_server_t* server;	   /**< Owning server instance. */
	worker_pool_t* owner_pool; /**< Per-worker pool that owns this client. */
	csilk_ctx_t ctx;	   /**< Request context for this connection. */
	size_t total_header_size;  /**< Total size of headers parsed so far. */
	size_t header_count;	   /**< Number of headers parsed so far. */

	/** @name Zero-Copy Parsing State
	 *  HTTP parser callbacks store direct references to the receive buffer
	 *  instead of allocating and copying. Eliminates all heap allocations
	 *  during header parsing. */
	/**@{*/
	csilk_str_view_t current_url;	       /**< Current URL (zero-copy ref to buf). */
	csilk_str_view_t current_header_field; /**< Current header field name. */
	csilk_str_view_t current_header_value; /**< Current header value. */
	/**@}*/

	SSL* ssl;		     /**< OpenSSL session object. */
	BIO* read_bio;		     /**< BIO for reading encrypted data. */
	BIO* write_bio;		     /**< BIO for writing encrypted data. */
	struct csilk_client_s* next; /**< Next client in active list. */
	struct csilk_client_s* prev; /**< Previous client in active list. */
};

/**
 * @brief Write data to the client's TCP socket, handling TLS encryption if necessary.
 * @param client The client connection.
 * @param data   The data to write.
 * @param length The length of the data.
 */
CSILK_INTERNAL void csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t length);

/**
 * @brief Dispatch a request context to the router and handler chain.
 * @param c The request context.
 */
CSILK_INTERNAL void _csilk_dispatch_request(csilk_ctx_t* c);

/**
 * @brief Internal: invoke all registered handlers for a given hook type.
 * @param s    The server instance.
 * @param c    The request context (may be nullptr for server-level hooks).
 * @param type Hook type to trigger.
 */
CSILK_INTERNAL void _csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type);

/**
 * @brief Persist a zero-copy header field+value pair into the request header map.
 *
 * Copies the header field and value from string views into the request arena
 * and inserts them into the request header hash map. This is the single point
 * where zero-copy references are materialized into persistent arena memory.
 *
 * @param c     Request context (for arena allocation).
 * @param field Header field name (zero-copy reference to recv buffer).
 * @param value Header value (zero-copy reference to recv buffer).
 */
CSILK_INTERNAL void
_csilk_persist_header(csilk_ctx_t* c, const csilk_str_view_t* field, const csilk_str_view_t* value);

/**
 * @brief Initialize arena subsystem with automatic TLS cleanup.
 *
 * Called once during server startup to register a pthread key destructor
 * that flushes the TLS chunk free list when threads exit.
 */
CSILK_INTERNAL void csilk_arena_init(void);

#endif /* CSILK_SERVER_INTERNAL_H */
