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
typedef struct ssl_st     SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct bio_st     BIO;

#include <stdatomic.h>
#include <csilk/core/sys_io.h>
#include <llhttp.h>
#include <nghttp2/nghttp2.h>

#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "../primitives/lfqueue.h"
#include "core/ctx/ctx_internal.h"

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
enum { MAX_TLS_CHUNKS_PER_TIER = 8 }; /* Max cached chunks per tier */
/** @brief Client connection object pool capacity.
 *
 * Controls the maximum number of idle client objects cached for reuse.
 * When pool is exhausted, new clients are allocated from heap and returned
 * to pool on connection close. Increase for high-concurrency scenarios. */
enum { CSILK_CLIENT_POOL_SIZE = 32 };

/** @brief Read buffer pool: three tiers for different request sizes.
 *  Buffers are per-worker (thread-local), pre-allocated at startup,
 *  and returned to the pool after each use. */
enum {
    CSILK_READ_BUF_TIER_COUNT = 3,
    CSILK_READ_BUF_4KB = 4096,
    CSILK_READ_BUF_16KB = 16384,
    CSILK_READ_BUF_64KB = 65536,
    CSILK_READ_BUF_POOL_SIZE = 64 /**< Per-tier pool depth. */
};
enum { CSILK_DISPATCH_TASK_SLAB_SIZE = 256 };

/** @brief Hook handler node in a linked list. */
typedef struct csilk_hook_node_s {
    void*                     handler;
    struct csilk_hook_node_s* next;
} csilk_hook_node_t;

/** @brief Protocol type for a client connection. */
typedef enum { CSILK_PROTO_UNKNOWN, CSILK_PROTO_HTTP1, CSILK_PROTO_HTTP2 } csilk_protocol_t;

/** @brief Connection lifecycle states. */
typedef enum {
    CSILK_CONN_INIT = 0,
    CSILK_CONN_ACCEPTED,
    CSILK_CONN_TLS,
    CSILK_CONN_READING,
    CSILK_CONN_PROCESSING,
    CSILK_CONN_WRITING,
    CSILK_CONN_STREAMING,
    CSILK_CONN_CLOSING,
    CSILK_CONN_CLOSED
} csilk_conn_state_t;

/** @brief Forward declaration for client connection structure. */
typedef struct csilk_client_s csilk_client_t;

/** @brief Task node for cross-thread dispatching. */
typedef struct csilk_dispatch_task_s {
    csilk_lfq_node_t                       lfq_node;  /**< Lock-free queue node (must be first). */
    _Atomic(struct csilk_dispatch_task_s*) pool_next; /**< Lock-free task pool next pointer. */
    void (*cb)(void* arg);
    void*           arg;
    csilk_client_t* client; /**< Associated client connection for lifetime safety. */
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
typedef struct CSILK_CACHE_ALIGNED {
    csilk_server_t*  server;        /**< Owning server instance. */
    csilk_io_loop_t  loop;          /**< This worker's I/O event loop (libuv or io_uring). */
    csilk_io_loop_t* loop_ptr;      /**< Pointer to the active loop. */
    csilk_io_tcp_t   server_handle; /**< Worker-local listen handle. */
    csilk_client_t*  client_pool[CSILK_CLIENT_POOL_SIZE]; /**< Worker-local free list. */
    int              client_pool_count;                   /**< Items in local free list. */
    csilk_io_async_t stop_async;                          /**< Async for graceful worker stop. */
    int              worker_index;                       /**< 0 = main loop, 1+ = worker threads. */
    csilk_arena_t*   arena_pool[CSILK_CLIENT_POOL_SIZE]; /**< Pre-allocated arena pool. */
    int              arena_pool_count;                   /**< Items in arena pool. */
    void*            read_buf_tiers[CSILK_READ_BUF_TIER_COUNT][CSILK_READ_BUF_POOL_SIZE];
    /**< Per-tier read buffer pool. */
    int read_buf_counts[CSILK_READ_BUF_TIER_COUNT];
    /**< Items in each tier's free list. */
    csilk_io_async_t dispatch_async; /**< Cross-thread task dispatch async handle. */
    csilk_lfqueue_t  dispatch_queue; /**< Lock-free MPSC dispatch queue. */
    csilk_client_t*  active_clients; /**< Head of worker-local active connections list. */
    void*            thread_pool;    /**< io_uring thread pool (NULL in libuv mode). */
} worker_pool_t;

#define CSILK_RELOAD_MAX_READERS 256

/**
 * @brief Per-reader epoch tracking slot aligned to 64-byte cache line.
 */
typedef struct csilk_rcu_slot_s {
    _Atomic(uint64_t)  active_epoch;  /**< 0 = inactive, >0 = epoch when entered */
    _Atomic(uintptr_t) owner_tid;     /**< Owner thread ID */
    _Atomic(uint32_t)  nesting_depth; /**< Reentrant depth */
    char               _pad[44];      /**< Pad to 64-byte cache line (64 - 8 - 8 - 4) */
} csilk_rcu_slot_t;

/**
 * @brief Node in the retired router list awaiting grace period completion.
 */
typedef struct csilk_retired_router_s {
    csilk_router_t* router;        /**< Router instance to be freed. */
    void*           dl_handle;     /**< dlopen() handle to dlclose (if dynamic). */
    char*           tmp_path;      /**< Temp file path to unlink (if any). */
    uint64_t        retired_epoch; /**< Epoch when retired. */
    _Atomic(struct csilk_retired_router_s*) retired_next; /**< Singly linked retired list. */
} csilk_retired_router_t;

/**
 * @brief RCU / EBR Manager for atomic router publishing and safe reclamation.
 */
typedef struct csilk_reload_mgr_s {
    _Atomic(uint64_t) global_epoch;  /**< Monotonically increasing epoch (starts at 1). */
    _Atomic(uint32_t) reclaim_lock;  /**< Lock-free reclamation mutual exclusion lock. */
    _Atomic(uint32_t) retired_count; /**< Count of retired routers. */
    _Atomic(csilk_retired_router_t*) retired_head; /**< Singly-linked list of retired routers. */
    csilk_rcu_slot_t                 reader_slots[CSILK_RELOAD_MAX_READERS]; /**< Epoch slots. */
} csilk_reload_mgr_t;

/**
 * @brief Main Server structure — represents the core HTTP server instance.
 *
 * Manages the I/O event loop (libuv or io_uring), HTTP listener, configuration, global
 * middleware chain, hook registrations, and client connection pooling.
 * Thread-safe for multi-threaded operation via atomic counters and mutexes.
 */
struct csilk_server_s {
    csilk_io_loop_t*         loop;             /**< I/O event loop (libuv or io_uring). */
    int                      loop_owned;       /**< 1 if loop was allocated by server_new. */
    _Atomic(csilk_router_t*) router;           /**< Associated router instance (atomic pointer). */
    csilk_reload_mgr_t       reload_mgr;       /**< Safe hot reload & EBR manager. */
    void*                    hot_reload_ctx;   /**< Hot reload file watcher context. */
    csilk_io_tcp_t           server_handle;    /**< TCP server handle. */
    csilk_io_signal_t        sig_handle;       /**< SIGINT signal handler. */
    csilk_io_async_t         async_handle;     /**< Async handle for cross-thread wakeup. */
    llhttp_settings_t        settings;         /**< HTTP parser callback settings. */
    csilk_server_config_t    config;           /**< Server configuration. */
    csilk_handler_t          middlewares[32];  /**< Global middlewares. */
    int                      middleware_count; /**< Number of global middlewares. */
    atomic_int               max_connections;  /**< Max concurrent connections (0=unlimited). */
    atomic_int               active_connections; /**< Current connection count (atomic). */
    csilk_thread_t*          worker_tids;        /**< Worker thread IDs (NULL if single-thread). */
    int                      worker_count;       /**< Number of worker threads created. */
    worker_pool_t*
        worker_pools;      /**< Per-worker pools (size = worker_threads, index 0 = main loop). */
    int worker_pool_count; /**< Number of worker pools (= worker_threads). */
    csilk_handler_t         not_found_handler; /**< Custom 404 handler (NULL = default). */
    char*                   spa_doc_root;      /**< SPA fallback doc root (NULL = disabled). */
    csilk_storage_driver_t* storage_driver;    /**< Context storage driver. */
    csilk_crypto_driver_t*  crypto_driver;     /**< Crypto algorithm driver. */
    csilk_cipher_driver_t*  cipher_driver;     /**< Cipher algorithm driver. */
    SSL_CTX*                ssl_ctx;           /**< OpenSSL context. */
    csilk_mq_t*             mq;                /**< Message Queue instance. */
    csilk_hook_node_t*      hooks[CSILK_HOOK_COUNT]; /**< Registered hooks. */
    void*                   quic_transport; /**< Optional QUIC transport callbacks for HTTP/3. */
};

/** @brief Client connection structure — represents a single TCP connection.
 *
 * Holds the I/O stream handle (libuv or io_uring), HTTP parser state, timers for keep-alive
 * and timeouts, TLS context (if HTTPS), and the request/response context.
 * Clients are pooled and reused for performance.
 */
struct csilk_client_s {
    uint64_t           generation;
    csilk_conn_state_t state;        /**< Connection lifecycle state machine. */

    csilk_io_tcp_t handle;           /**< I/O stream handle (libuv or io_uring). */

    csilk_io_timer_t timer;          /**< Connection idle (keep-alive) timer. */
    csilk_io_timer_t read_timer;     /**< Read timeout timer. */
    csilk_io_timer_t write_timer;    /**< Write timeout timer. */
    csilk_io_timer_t request_timer;  /**< Request timeout timer. */
    _Atomic int      ref_count;      /**< Unified connection reference count. */
    _Atomic int      pending_io;     /**< In-flight I/O operations and closing timer handles. */
    int              read_paused;
    unsigned         read_active : 1;
    unsigned         keep_alive : 1; /**< Cached keep-alive decision from
                   * _csilk_send_response, used by on_write_done
                   * because llhttp clears F_CONNECTION_CLOSE
                   * after on_message_complete returns. */
    void*            read_buf;       /**< Pre-allocated read buffer for io_uring */
    size_t pending_write_bytes;  /**< In-flight outbound write bytes for io_uring/backpressure. */

    csilk_protocol_t protocol;   /**< Protocol negotiated for this connection. */

    nghttp2_session* h2_session; /**< HTTP/2 session state (if HTTP/2). */
    csilk_ctx_t*     h2_streams; /**< Linked list of active HTTP/2 stream contexts. */

    llhttp_t parser;             /**< HTTP request parser (if HTTP/1.1). */

    csilk_server_t* server;      /**< Owning server instance. */
    worker_pool_t*  owner_pool;  /**< Per-worker pool that owns this client. */
    csilk_ctx_t     ctx;         /**< Request context for this connection. */
    size_t          total_header_size; /**< Total size of headers parsed so far. */
    size_t          header_count;      /**< Number of headers parsed so far. */

    /** @name Zero-Copy Parsing State
     *  HTTP parser callbacks store direct references to the receive buffer
     *  instead of allocating and copying. Eliminates all heap allocations
     *  during header parsing. */
    /**@{*/
    csilk_str_view_t current_url;          /**< Current URL (zero-copy ref to buf). */
    csilk_str_view_t current_header_field; /**< Current header field name. */
    csilk_str_view_t current_header_value; /**< Current header value. */
    uint8_t header_field_completed;        /**< Whether current header field parsing is complete. */
    /**@}*/

    SSL*                   ssl;       /**< OpenSSL session object. */
    BIO*                   read_bio;  /**< BIO for reading encrypted data. */
    BIO*                   write_bio; /**< BIO for writing encrypted data. */
    struct csilk_client_s* next;      /**< Next client in active list. */
    struct csilk_client_s* prev;      /**< Previous client in active list. */
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
 * @param c    The request context (may be NULL for server-level hooks).
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

/** @brief Return a read buffer to the worker-local pool; falls back to free. */
CSILK_INTERNAL void pool_put_read_buf(worker_pool_t* wp, char* base, size_t size);

/** @brief Pre-allocate per-tier read buffers at worker startup. */
CSILK_INTERNAL void _csilk_worker_init_read_buf_pool(worker_pool_t* wp);

/**
 * @brief Initialize arena subsystem with automatic TLS cleanup.
 *
 * Called once during server startup to register a pthread key destructor
 * that flushes the TLS chunk free list when threads exit.
 */
CSILK_INTERNAL void csilk_arena_init(void);

/** @brief Initialize the RCU / EBR reload manager on a server. */
CSILK_INTERNAL void _csilk_reload_mgr_init(csilk_server_t* server);

/** @brief Try to reclaim retired routers and dynamic libraries whose grace period has expired. */
CSILK_INTERNAL void _csilk_reload_try_reclaim(csilk_server_t* server);

/** @brief Destroy the reload manager and force reclamation of all remaining records. */
CSILK_INTERNAL void _csilk_reload_mgr_free(csilk_server_t* server);

#endif /* CSILK_SERVER_INTERNAL_H */
