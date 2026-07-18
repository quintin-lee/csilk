"""Low-level ctypes bindings for the csilk C library.

This module is the bridge between Python and the native ``libcsilk.so`` / ``.dylib``.
It defines all ``ctypes.Structure`` subclasses for C structs, callback function
types (``CFUNCTYPE``), and the ``get_bindings()`` function that lazily loads the
shared library and assigns ``argtypes`` / ``restype`` on every C function.

Most users should **not** import this module directly; use the high-level
wrappers in ``app``, ``context``, ``db``, ``ai``, ``workflow``, etc.
"""

import ctypes
import os
import sys

# Define opaque types
class CsilkApp(ctypes.Structure):
    pass
class CsilkCtx(ctypes.Structure):
    pass
class CsilkServer(ctypes.Structure):
    pass

CsilkAppPtr = ctypes.POINTER(CsilkApp)
CsilkCtxPtr = ctypes.POINTER(CsilkCtx)
CsilkServerPtr = ctypes.POINTER(CsilkServer)

class CsilkCircuitBreaker(ctypes.Structure):
    pass
CsilkCircuitBreakerPtr = ctypes.POINTER(CsilkCircuitBreaker)

class CsilkCircuitBreakerConfig(ctypes.Structure):
    _fields_ = [
        ("failure_threshold", ctypes.c_int),
        ("recovery_timeout_ms", ctypes.c_int)
    ]
CsilkCircuitBreakerConfigPtr = ctypes.POINTER(CsilkCircuitBreakerConfig)

class CsilkSlidingLimiter(ctypes.Structure):
    pass
CsilkSlidingLimiterPtr = ctypes.POINTER(CsilkSlidingLimiter)

class CsilkCorsConfig(ctypes.Structure):
    _fields_ = [
        ("allow_origin", ctypes.c_char_p),
        ("allow_methods", ctypes.c_char_p),
        ("allow_headers", ctypes.c_char_p),
        ("allow_credentials", ctypes.c_int),
        ("max_age", ctypes.c_int)
    ]

class CsilkData(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_char_p),
        ("value", ctypes.c_void_p),
        ("free_fn", ctypes.c_void_p),
        ("meta", ctypes.c_void_p)
    ]
CsilkDataPtr = ctypes.POINTER(CsilkData)

class CsilkAiMeta(ctypes.Structure):
    _fields_ = [
        ("model", ctypes.c_char_p),
        ("prompt_tokens", ctypes.c_int),
        ("completion_tokens", ctypes.c_int)
    ]
CsilkAiMetaPtr = ctypes.POINTER(CsilkAiMeta)


class CsilkAiConfig(ctypes.Structure):
    _fields_ = [
        ("model", ctypes.c_char_p),
        ("system_msg", ctypes.c_char_p),
        ("prompt", ctypes.c_char_p),
        ("temperature", ctypes.c_double),
        ("max_tokens", ctypes.c_int),
        ("stream", ctypes.c_int),
        ("max_history_messages", ctypes.c_int)
    ]
CsilkAiConfigPtr = ctypes.POINTER(CsilkAiConfig)

class CsilkVectorSearchConfig(ctypes.Structure):
    _fields_ = [
        ("ai", ctypes.c_void_p),
        ("embedding_model", ctypes.c_char_p),
        ("db", ctypes.c_void_p),
        ("collection", ctypes.c_char_p),
        ("limit", ctypes.c_int),
        ("input_template", ctypes.c_char_p)
    ]
CsilkVectorSearchConfigPtr = ctypes.POINTER(CsilkVectorSearchConfig)

class CsilkWfToolEntry(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("description", ctypes.c_char_p),
        ("parameters_json", ctypes.c_char_p),
        ("fn", ctypes.c_void_p),
        ("user_data", ctypes.c_void_p)
    ]
CsilkWfToolEntryPtr = ctypes.POINTER(CsilkWfToolEntry)
class CsilkAiMessage(ctypes.Structure):
    _fields_ = [
        ("role", ctypes.c_char_p),
        ("content", ctypes.c_char_p)
    ]

class CsilkAiToolCall(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char_p),
        ("name", ctypes.c_char_p),
        ("arguments", ctypes.c_char_p)
    ]
CsilkAiToolCallPtr = ctypes.POINTER(CsilkAiToolCall)

class CsilkAiChatResponse(ctypes.Structure):
    _fields_ = [
        ("content", ctypes.c_char_p),
        ("tool_calls", CsilkAiToolCallPtr),
        ("tool_call_count", ctypes.c_size_t),
        ("prompt_tokens", ctypes.c_int),
        ("completion_tokens", ctypes.c_int),
        ("total_tokens", ctypes.c_int),
        ("raw_response", ctypes.c_char_p),
        ("error_message", ctypes.c_char_p)
    ]

class CsilkAiEmbeddingsResponse(ctypes.Structure):
    _fields_ = [
        ("values", ctypes.POINTER(ctypes.c_float)),
        ("dimension", ctypes.c_size_t),
        ("count", ctypes.c_size_t),
        ("prompt_tokens", ctypes.c_int),
        ("total_tokens", ctypes.c_int),
        ("error_message", ctypes.c_char_p)
    ]

class CsilkAiToolFunction(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("description", ctypes.c_char_p),
        ("parameters_json", ctypes.c_void_p)
    ]

class CsilkAiTool(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_char_p),
        ("function", CsilkAiToolFunction)
    ]
CsilkAiToolPtr = ctypes.POINTER(CsilkAiTool)

class CsilkAiChatRequest(ctypes.Structure):
    _fields_ = [
        ("model", ctypes.c_char_p),
        ("messages", ctypes.POINTER(CsilkAiMessage)),
        ("message_count", ctypes.c_size_t),
        ("temperature", ctypes.c_double),
        ("top_p", ctypes.c_double),
        ("presence_penalty", ctypes.c_double),
        ("frequency_penalty", ctypes.c_double),
        ("max_tokens", ctypes.c_int),
        ("stop", ctypes.POINTER(ctypes.c_char_p)),
        ("stop_count", ctypes.c_size_t),
        ("user", ctypes.c_char_p),
        ("stream", ctypes.c_bool),
        ("on_chunk", ctypes.c_void_p),
        ("user_data", ctypes.c_void_p),
        ("timeout_ms", ctypes.c_int),
        ("tools", CsilkAiToolPtr),
        ("tool_count", ctypes.c_size_t),
        ("tool_choice", ctypes.c_char_p)
    ]

class CsilkAiContext(ctypes.Structure):
    _fields_ = [
        ("messages", ctypes.POINTER(CsilkAiMessage)),
        ("count", ctypes.c_size_t),
        ("capacity", ctypes.c_size_t),
        ("max_history", ctypes.c_size_t)
    ]

class CsilkAiStats(ctypes.Structure):
    _fields_ = [
        ("requests_total", ctypes.c_uint64),
        ("tokens_total", ctypes.c_uint64),
        ("prompt_tokens", ctypes.c_uint64),
        ("completion_tokens", ctypes.c_uint64),
        ("errors_total", ctypes.c_uint64),
        ("duration_us_total", ctypes.c_uint64)
    ]

class CsilkDbStats(ctypes.Structure):
    _fields_ = [
        ("queries_total", ctypes.c_uint64),
        ("execs_total", ctypes.c_uint64),
        ("errors_total", ctypes.c_uint64),
        ("duration_us_total", ctypes.c_uint64)
    ]

CsilkAiStreamCb = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)

CsilkHandler = ctypes.CFUNCTYPE(None, CsilkCtxPtr)
CsilkWsMessageCallback = ctypes.CFUNCTYPE(None, CsilkCtxPtr, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int)
CsilkHeaderCb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p)
CsilkMqHandler = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

CsilkWfHandler = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, CsilkDataPtr, ctypes.c_void_p)
CsilkWfRouter = ctypes.CFUNCTYPE(ctypes.c_char_p, CsilkDataPtr)
CsilkWfToolFn = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
CsilkWfRunCallback = ctypes.CFUNCTYPE(None, CsilkDataPtr)
CsilkWfRunTracedCallback = ctypes.CFUNCTYPE(None, CsilkDataPtr, ctypes.c_void_p)
CsilkWfToolDiscovery = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_void_p
)

class CsilkMultipartPart(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 128),
        ("filename", ctypes.c_char * 256),
        ("content_type", ctypes.c_char * 64),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("data_len", ctypes.c_size_t),
        ("ctx", ctypes.c_void_p)
    ]
CsilkMultipartPartPtr = ctypes.POINTER(CsilkMultipartPart)

CsilkMultipartHandler = ctypes.CFUNCTYPE(None, CsilkMultipartPartPtr)

class CsilkMqStats(ctypes.Structure):
    _fields_ = [
        ("published_total", ctypes.c_uint64),
        ("delivered_total", ctypes.c_uint64),
        ("failed_total", ctypes.c_uint64),
        ("queue_depth", ctypes.c_uint32),
        ("topic_count", ctypes.c_uint32)
    ]

class CsilkVectorPoint(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char_p),
        ("vector", ctypes.POINTER(ctypes.c_float)),
        ("dimension", ctypes.c_size_t),
        ("payload", ctypes.c_void_p)
    ]

class CsilkVectorSearchResult(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char_p),
        ("score", ctypes.c_float),
        ("payload", ctypes.c_void_p)
    ]

class CsilkVectorSearchResponse(ctypes.Structure):
    _fields_ = [
        ("results", ctypes.POINTER(CsilkVectorSearchResult)),
        ("count", ctypes.c_size_t),
        ("error_message", ctypes.c_char_p)
    ]

class CsilkServerConfig(ctypes.Structure):
    _fields_ = [
        ("idle_timeout_ms", ctypes.c_uint),
        ("read_timeout_ms", ctypes.c_uint),
        ("write_timeout_ms", ctypes.c_uint),
        ("request_timeout_ms", ctypes.c_uint),
        ("max_body_size", ctypes.c_size_t),
        ("max_header_size", ctypes.c_size_t),
        ("max_url_size", ctypes.c_size_t),
        ("max_headers_count", ctypes.c_size_t),
        ("max_connections", ctypes.c_int),
        ("listen_backlog", ctypes.c_int),
        ("tcp_nodelay", ctypes.c_int),
        ("tcp_keepalive", ctypes.c_int),
        ("worker_threads", ctypes.c_int),
        
        ("enable_tls", ctypes.c_int),
        ("tls_cert_file", ctypes.c_char_p),
        ("tls_key_file", ctypes.c_char_p),
        ("tls_ca_file", ctypes.c_char_p),
        ("tls_verify_peer", ctypes.c_int),
        
        ("h2_push_enable", ctypes.c_int),
        ("h2_max_push_per_request", ctypes.c_int),
        
        ("enable_simd", ctypes.c_int),
        ("enable_arena_alignment", ctypes.c_int),
        
        ("enable_openapi", ctypes.c_int)
    ]

_cached_lib = None

def load_lib():
    env_path = os.environ.get("LIBCSILK_PATH")
    if env_path:
        return ctypes.CDLL(env_path)
    
    if sys.platform.startswith("win"):
        lib_name = "csilk.dll"
    elif sys.platform == "darwin":
        lib_name = "libcsilk.dylib"
    else:
        lib_name = "libcsilk.so"

    # 1. Look inside the current package directory (for wheels)
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    pkg_path = os.path.join(pkg_dir, lib_name)
    if os.path.exists(pkg_path):
        return ctypes.CDLL(pkg_path)
        
    # 2. Look in the build directory (for local dev)
    base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_path = os.path.join(base_dir, "build", lib_name)
    
    if os.path.exists(build_path):
        return ctypes.CDLL(build_path)
    
    # 3. Fallback to system paths
    try:
        return ctypes.CDLL(lib_name)
    except OSError as e:
        raise OSError(f"Could not load {lib_name}. Please compile csilk as shared library and configure LIBCSILK_PATH.") from e

def get_bindings():
    global _cached_lib
    if _cached_lib is not None:
        return _cached_lib
    
    lib = load_lib()
    
    # App management
    lib.csilk_app_new.restype = CsilkAppPtr
    lib.csilk_app_new.argtypes = [ctypes.c_char_p]
    
    lib.csilk_app_free.restype = None
    lib.csilk_app_free.argtypes = [CsilkAppPtr]
    
    lib.csilk_app_add_route.restype = None
    lib.csilk_app_add_route.argtypes = [CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler]
    
    lib.csilk_app_static.restype = None
    lib.csilk_app_static.argtypes = [CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    lib.csilk_app_enable_openapi.restype = None
    lib.csilk_app_enable_openapi.argtypes = [CsilkAppPtr, ctypes.c_int]

    lib.csilk_app_set_server_config.restype = None
    lib.csilk_app_set_server_config.argtypes = [CsilkAppPtr, CsilkServerConfig]
    
    # Logger
    lib.csilk_app_log_level.restype = None
    lib.csilk_app_log_level.argtypes = [CsilkAppPtr, ctypes.c_int]
    
    lib.csilk_app_log_file.restype = None
    lib.csilk_app_log_file.argtypes = [CsilkAppPtr, ctypes.c_char_p, ctypes.c_size_t]
    
    lib.csilk_app_log_json.restype = None
    lib.csilk_app_log_json.argtypes = [CsilkAppPtr, ctypes.c_int]
    
    # Middleware application
    lib.csilk_app_use.restype = None
    lib.csilk_app_use.argtypes = [CsilkAppPtr, CsilkHandler]
    
    lib.csilk_app_use_group.restype = None
    lib.csilk_app_use_group.argtypes = [CsilkAppPtr, ctypes.c_char_p, CsilkHandler]
    
    lib.csilk_app_apply_config.restype = None
    lib.csilk_app_apply_config.argtypes = [CsilkAppPtr]
    
    lib.csilk_app_run.restype = ctypes.c_int
    lib.csilk_app_run.argtypes = [CsilkAppPtr, ctypes.c_int]
    
    lib.csilk_app_server.restype = CsilkServerPtr
    lib.csilk_app_server.argtypes = [CsilkAppPtr]

    lib.csilk_app_router.restype = ctypes.c_void_p
    lib.csilk_app_router.argtypes = [CsilkAppPtr]
    
    # Server management
    lib.csilk_server_stop.restype = None
    lib.csilk_server_stop.argtypes = [CsilkServerPtr]

    # Route Groups
    lib.csilk_group_new.restype = ctypes.c_void_p
    lib.csilk_group_new.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_group_group.restype = ctypes.c_void_p
    lib.csilk_group_group.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_group_use.restype = None
    lib.csilk_group_use.argtypes = [ctypes.c_void_p, CsilkHandler]

    lib.csilk_group_add_route_extended_perm.restype = None
    lib.csilk_group_add_route_extended_perm.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_char_p, ctypes.c_char_p
    ]

    lib.csilk_group_free.restype = None
    lib.csilk_group_free.argtypes = [ctypes.c_void_p]
    
    # Context getters
    lib.csilk_get_method.restype = ctypes.c_char_p
    lib.csilk_get_method.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_path.restype = ctypes.c_char_p
    lib.csilk_get_path.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_body.restype = ctypes.c_void_p
    lib.csilk_get_body.argtypes = [CsilkCtxPtr, ctypes.POINTER(ctypes.c_size_t)]
    
    lib.csilk_get_body_len.restype = ctypes.c_size_t
    lib.csilk_get_body_len.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_query.restype = ctypes.c_char_p
    lib.csilk_get_query.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_get_param.restype = ctypes.c_char_p
    lib.csilk_get_param.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    # Context path parameters iterator
    lib.csilk_get_params_count.restype = ctypes.c_int
    lib.csilk_get_params_count.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_param_key.restype = ctypes.c_char_p
    lib.csilk_get_param_key.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    lib.csilk_get_param_value.restype = ctypes.c_char_p
    lib.csilk_get_param_value.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    # Context states
    lib.csilk_is_websocket.restype = ctypes.c_int
    lib.csilk_is_websocket.argtypes = [CsilkCtxPtr]
    
    lib.csilk_is_sse.restype = ctypes.c_int
    lib.csilk_is_sse.argtypes = [CsilkCtxPtr]
    
    lib.csilk_is_aborted.restype = ctypes.c_int
    lib.csilk_is_aborted.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_header.restype = ctypes.c_char_p
    lib.csilk_get_header.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_get_response_header.restype = ctypes.c_char_p
    lib.csilk_get_response_header.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_get_request_id.restype = ctypes.c_char_p
    lib.csilk_get_request_id.argtypes = [CsilkCtxPtr]
    
    lib.csilk_set_request_id.restype = None
    lib.csilk_set_request_id.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_set_request_header.restype = None
    lib.csilk_set_request_header.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    lib.csilk_get_status.restype = ctypes.c_int
    lib.csilk_get_status.argtypes = [CsilkCtxPtr]
    
    lib.csilk_set_status.restype = None
    lib.csilk_set_status.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    lib.csilk_ctx_set_websocket.restype = None
    lib.csilk_ctx_set_websocket.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    lib.csilk_ctx_set_sse.restype = None
    lib.csilk_ctx_set_sse.argtypes = [CsilkCtxPtr, ctypes.c_int]

    lib.csilk_ctx_set_async.restype = None
    lib.csilk_ctx_set_async.argtypes = [CsilkCtxPtr, ctypes.c_int]

    lib.csilk_is_async.restype = ctypes.c_int
    lib.csilk_is_async.argtypes = [CsilkCtxPtr]

    CsilkDispatchCb = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
    lib.csilk_dispatch.restype = None
    lib.csilk_dispatch.argtypes = [CsilkCtxPtr, CsilkDispatchCb, ctypes.c_void_p]
    
    # Response helpers
    lib.csilk_status.restype = None
    lib.csilk_status.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    lib.csilk_string.restype = None
    lib.csilk_string.argtypes = [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]
    
    lib.csilk_set_header.restype = None
    lib.csilk_set_header.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    lib.csilk_file.restype = None
    lib.csilk_file.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_redirect.restype = None
    lib.csilk_redirect.argtypes = [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]

    lib.csilk_add_header.restype = None
    lib.csilk_add_header.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_json_error.restype = None
    lib.csilk_json_error.argtypes = [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]

    lib.csilk_redirect_simple.restype = None
    lib.csilk_redirect_simple.argtypes = [CsilkCtxPtr, ctypes.c_char_p]

    lib.csilk_response_write.restype = None
    lib.csilk_response_write.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_size_t]

    lib.csilk_response_end.restype = None
    lib.csilk_response_end.argtypes = [CsilkCtxPtr]

    lib.csilk_push_promise.restype = ctypes.c_int32
    lib.csilk_push_promise.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    # Middleware flow
    lib.csilk_next.restype = None
    lib.csilk_next.argtypes = [CsilkCtxPtr]
    
    lib.csilk_abort.restype = None
    lib.csilk_abort.argtypes = [CsilkCtxPtr]
    
    # WebSockets
    lib.csilk_ws_handshake.restype = None
    lib.csilk_ws_handshake.argtypes = [CsilkCtxPtr]
    
    lib.csilk_ws_send.restype = None
    lib.csilk_ws_send.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
    
    lib.csilk_ws_close.restype = None
    lib.csilk_ws_close.argtypes = [CsilkCtxPtr, ctypes.c_uint16, ctypes.c_char_p]
    
    lib.csilk_ws_join_room.restype = None
    lib.csilk_ws_join_room.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_ws_leave_room.restype = None
    lib.csilk_ws_leave_room.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_ws_broadcast_room.restype = None
    lib.csilk_ws_broadcast_room.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    # SSE
    lib.csilk_sse_init.restype = None
    lib.csilk_sse_init.argtypes = [CsilkCtxPtr]
    
    lib.csilk_sse_send.restype = None
    lib.csilk_sse_send.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    lib.csilk_sse_close.restype = None
    lib.csilk_sse_close.argtypes = [CsilkCtxPtr]
    
    # WebSocket callbacks
    lib.csilk_set_on_ws_message.restype = None
    lib.csilk_set_on_ws_message.argtypes = [CsilkCtxPtr, CsilkWsMessageCallback]
    
    lib.csilk_get_on_ws_message.restype = ctypes.c_void_p
    lib.csilk_get_on_ws_message.argtypes = [CsilkCtxPtr]
    
    # Server hooks
    lib.csilk_server_add_hook.restype = None
    lib.csilk_server_add_hook.argtypes = [CsilkServerPtr, ctypes.c_int, ctypes.c_void_p]
    
    lib.csilk_server_set_not_found_handler.restype = None
    lib.csilk_server_set_not_found_handler.argtypes = [CsilkServerPtr, CsilkHandler]
    
    lib.csilk_server_set_spa_fallback.restype = None
    lib.csilk_server_set_spa_fallback.argtypes = [CsilkServerPtr, ctypes.c_char_p]
    
    lib.csilk_server_get_stats.restype = None
    lib.csilk_server_get_stats.argtypes = [CsilkServerPtr, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    
    lib.csilk_server_set_max_connections.restype = ctypes.c_int
    lib.csilk_server_set_max_connections.argtypes = [CsilkServerPtr, ctypes.c_int]
    
    # Extended routing with metadata
    lib.csilk_app_add_route_extended.restype = None
    lib.csilk_app_add_route_extended.argtypes = [
        CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p
    ]
    
    lib.csilk_app_add_route_perm.restype = None
    lib.csilk_app_add_route_perm.argtypes = [
        CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
        ctypes.c_char_p, ctypes.c_char_p
    ]
    
    lib.csilk_app_add_route_extended_perm.restype = None
    lib.csilk_app_add_route_extended_perm.argtypes = [
        CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_char_p, ctypes.c_char_p
    ]
    
    # Utilities
    lib.csilk_free.restype = None
    lib.csilk_free.argtypes = [ctypes.c_void_p]
    
    # Cookies & Forms
    lib.csilk_get_cookie.restype = ctypes.c_char_p
    lib.csilk_get_cookie.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_set_cookie.restype = None
    lib.csilk_set_cookie.argtypes = [
        CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int
    ]
    
    lib.csilk_parse_form_urlencoded.restype = None
    lib.csilk_parse_form_urlencoded.argtypes = [CsilkCtxPtr]
    
    # Hash Map Iteration
    lib.csilk_for_each_header.restype = None
    lib.csilk_for_each_header.argtypes = [CsilkCtxPtr, CsilkHeaderCb, ctypes.c_void_p]
    
    lib.csilk_for_each_query.restype = None
    lib.csilk_for_each_query.argtypes = [CsilkCtxPtr, CsilkHeaderCb, ctypes.c_void_p]
    
    lib.csilk_for_each_form_field.restype = None
    lib.csilk_for_each_form_field.argtypes = [CsilkCtxPtr, CsilkHeaderCb, ctypes.c_void_p]
    
    # Context Handler Info
    lib.csilk_ctx_get_handler_path.restype = ctypes.c_char_p
    lib.csilk_ctx_get_handler_path.argtypes = [CsilkCtxPtr]
    
    lib.csilk_ctx_get_handler_perm_required.restype = ctypes.c_char_p
    lib.csilk_ctx_get_handler_perm_required.argtypes = [CsilkCtxPtr]
    
    lib.csilk_ctx_get_handler_perm_resource.restype = ctypes.c_char_p
    lib.csilk_ctx_get_handler_perm_resource.argtypes = [CsilkCtxPtr]
    
    # Request-local context storage (opaque pointers)
    lib.csilk_set.restype = None
    lib.csilk_set.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_get.restype = ctypes.c_void_p
    lib.csilk_get.argtypes = [CsilkCtxPtr, ctypes.c_char_p]

    # Test utilities
    lib.csilk_test_ctx_new.restype = CsilkCtxPtr
    lib.csilk_test_ctx_new.argtypes = []

    lib.csilk_test_ctx_free.restype = None
    lib.csilk_test_ctx_free.argtypes = [CsilkCtxPtr]

    # Storage functions
    lib.csilk_set_string.restype = ctypes.c_int
    lib.csilk_set_string.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    
    lib.csilk_get_string.restype = ctypes.c_void_p
    lib.csilk_get_string.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_incr.restype = ctypes.c_longlong
    lib.csilk_incr.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_int]
    
    # Response body mutation
    lib.csilk_set_response_body.restype = None
    lib.csilk_set_response_body.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
    
    lib.csilk_get_response_body.restype = ctypes.c_char_p
    lib.csilk_get_response_body.argtypes = [CsilkCtxPtr, ctypes.POINTER(ctypes.c_size_t)]
    
    # Built-in C Middlewares
    lib.csilk_recovery_handler.restype = None
    lib.csilk_recovery_handler.argtypes = [CsilkCtxPtr]
    
    lib.csilk_logger_handler.restype = None
    lib.csilk_logger_handler.argtypes = [CsilkCtxPtr]
    
    lib.csilk_waf_middleware.restype = None
    lib.csilk_waf_middleware.argtypes = [CsilkCtxPtr]
    
    lib.csilk_request_id_middleware.restype = None
    lib.csilk_request_id_middleware.argtypes = [CsilkCtxPtr]
    
    lib.csilk_health_check_handler.restype = None
    lib.csilk_health_check_handler.argtypes = [CsilkCtxPtr]
    
    lib.csilk_ready_check_handler.restype = None
    lib.csilk_ready_check_handler.argtypes = [CsilkCtxPtr]
    
    lib.csilk_rate_limit_middleware.restype = None
    lib.csilk_rate_limit_middleware.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    lib.csilk_csrf_middleware.restype = None
    lib.csilk_csrf_middleware.argtypes = [CsilkCtxPtr]
    
    lib.csilk_gzip_middleware.restype = None
    lib.csilk_gzip_middleware.argtypes = [CsilkCtxPtr]
    
    lib.csilk_cors_middleware.restype = None
    lib.csilk_cors_middleware.argtypes = [CsilkCtxPtr, ctypes.POINTER(CsilkCorsConfig)]

    lib.csilk_multipart_parse.restype = None
    lib.csilk_multipart_parse.argtypes = [CsilkCtxPtr, CsilkMultipartHandler]
    
    # JWT Middlewares & Helpers
    lib.csilk_jwt_middleware.restype = None
    lib.csilk_jwt_middleware.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_ctx_get_jwt_payload_json.restype = ctypes.c_void_p
    lib.csilk_ctx_get_jwt_payload_json.argtypes = [CsilkCtxPtr]
    
    lib.csilk_ctx_cleanup_jwt_payload.restype = None
    lib.csilk_ctx_cleanup_jwt_payload.argtypes = [CsilkCtxPtr]
    
    lib.csilk_jwt_generate_json.restype = ctypes.c_void_p
    lib.csilk_jwt_generate_json.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]

    
    # Session Management
    lib.csilk_session_init.restype = None
    lib.csilk_session_init.argtypes = []
    
    lib.csilk_session_start.restype = None
    lib.csilk_session_start.argtypes = [CsilkCtxPtr]
    
    lib.csilk_session_set.restype = None
    lib.csilk_session_set.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_void_p]
    
    lib.csilk_session_get.restype = ctypes.c_void_p
    lib.csilk_session_get.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_session_destroy.restype = None
    lib.csilk_session_destroy.argtypes = [CsilkCtxPtr]
    
    lib.csilk_session_get_id.restype = ctypes.c_char_p
    lib.csilk_session_get_id.argtypes = [CsilkCtxPtr]
    
    # Message Queue (MQ) Event Bus
    lib.csilk_server_get_mq.restype = ctypes.c_void_p
    lib.csilk_server_get_mq.argtypes = [CsilkServerPtr]
    
    lib.csilk_mq_use.restype = None
    lib.csilk_mq_use.argtypes = [ctypes.c_void_p, ctypes.c_char_p, CsilkMqHandler]
    
    lib.csilk_mq_subscribe.restype = None
    lib.csilk_mq_subscribe.argtypes = [ctypes.c_void_p, ctypes.c_char_p, CsilkMqHandler]
    
    lib.csilk_mq_publish.restype = ctypes.c_int
    lib.csilk_mq_publish.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
    
    lib.csilk_mq_next.restype = None
    lib.csilk_mq_next.argtypes = [ctypes.c_void_p]
    
    lib.csilk_mq_abort.restype = None
    lib.csilk_mq_abort.argtypes = [ctypes.c_void_p]
    
    lib.csilk_mq_get_topic.restype = ctypes.c_char_p
    lib.csilk_mq_get_topic.argtypes = [ctypes.c_void_p]
    
    lib.csilk_mq_get_payload.restype = ctypes.c_void_p
    lib.csilk_mq_get_payload.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
    
    lib.csilk_mq_get_stats.restype = None
    lib.csilk_mq_get_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(CsilkMqStats)]
    
    lib.csilk_mq_set_persistence.restype = ctypes.c_int
    lib.csilk_mq_set_persistence.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    
    # Admin Dashboard
    lib.csilk_admin_serve.restype = None
    lib.csilk_admin_serve.argtypes = [CsilkAppPtr, ctypes.c_char_p]
    
    lib.csilk_admin_serve_secure.restype = None
    lib.csilk_admin_serve_secure.argtypes = [CsilkAppPtr, ctypes.c_char_p, CsilkHandler]
    
    # Utilities
    lib.csilk_malloc.restype = ctypes.c_void_p
    lib.csilk_malloc.argtypes = [ctypes.c_size_t]

    lib.csilk_strdup.restype = ctypes.c_void_p
    lib.csilk_strdup.argtypes = [ctypes.c_char_p]

    # Workflow Engine
    lib.csilk_wf_serve_ui.restype = None
    lib.csilk_wf_serve_ui.argtypes = [CsilkAppPtr, ctypes.c_char_p]

    lib.csilk_wf_new.restype = ctypes.c_void_p
    lib.csilk_wf_new.argtypes = [ctypes.c_char_p]

    lib.csilk_wf_free.restype = None
    lib.csilk_wf_free.argtypes = [ctypes.c_void_p]

    lib.csilk_wf_add.restype = ctypes.c_void_p
    lib.csilk_wf_add.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_add_ai.restype = ctypes.c_void_p
    lib.csilk_wf_add_ai.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_add_vector_search.restype = ctypes.c_void_p
    lib.csilk_wf_add_vector_search.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_register_tool.restype = None
    lib.csilk_wf_register_tool.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_set_tool_discovery.restype = None
    lib.csilk_wf_set_tool_discovery.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_get_node.restype = ctypes.c_void_p
    lib.csilk_wf_get_node.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_wf_node_set_entry.restype = None
    lib.csilk_wf_node_set_entry.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.csilk_wf_bind.restype = None
    lib.csilk_wf_bind.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_on.restype = None
    lib.csilk_wf_on.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_on_loop.restype = None
    lib.csilk_wf_on_loop.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_on_error.restype = None
    lib.csilk_wf_on_error.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_route.restype = None
    lib.csilk_wf_route.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_node_set_join.restype = None
    lib.csilk_wf_node_set_join.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.csilk_wf_node_set_interactive.restype = None
    lib.csilk_wf_node_set_interactive.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.csilk_wf_node_set_schema.restype = None
    lib.csilk_wf_node_set_schema.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_wf_signal_continue.restype = None
    lib.csilk_wf_signal_continue.argtypes = [ctypes.c_void_p, ctypes.c_char_p, CsilkDataPtr, ctypes.c_void_p]

    lib.csilk_wf_node_set_timeout.restype = None
    lib.csilk_wf_node_set_timeout.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.csilk_wf_set_ttl.restype = None
    lib.csilk_wf_set_ttl.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.csilk_wf_node_set_retry.restype = None
    lib.csilk_wf_node_set_retry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

    lib.csilk_wf_node_set_remote.restype = None
    lib.csilk_wf_node_set_remote.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.csilk_wf_enable_distributed.restype = None
    lib.csilk_wf_enable_distributed.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.csilk_wf_set_persistence.restype = None
    lib.csilk_wf_set_persistence.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_wf_run.restype = ctypes.c_char_p
    lib.csilk_wf_run.argtypes = [ctypes.c_void_p, CsilkDataPtr, ctypes.c_void_p]

    lib.csilk_wf_resume.restype = None
    lib.csilk_wf_resume.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_run_traced.restype = None
    lib.csilk_wf_run_traced.argtypes = [ctypes.c_void_p, CsilkDataPtr, ctypes.c_void_p]

    lib.csilk_wf_trace_to_json.restype = ctypes.c_void_p
    lib.csilk_wf_trace_to_json.argtypes = [ctypes.c_void_p]

    lib.csilk_wf_trace_free.restype = None
    lib.csilk_wf_trace_free.argtypes = [ctypes.c_void_p]

    # Memory Helpers (Arena-backed)
    lib.csilk_wf_data_new.restype = CsilkDataPtr
    lib.csilk_wf_data_new.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_strdup.restype = ctypes.c_void_p
    lib.csilk_wf_strdup.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_wf_alloc.restype = ctypes.c_void_p
    lib.csilk_wf_alloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    # Declarative API
    lib.csilk_wf_register_handler.restype = None
    lib.csilk_wf_register_handler.argtypes = [ctypes.c_char_p, ctypes.c_void_p]

    lib.csilk_wf_load_yaml.restype = ctypes.c_void_p
    lib.csilk_wf_load_yaml.argtypes = [ctypes.c_char_p]

    lib.csilk_wf_from_json.restype = ctypes.c_void_p
    lib.csilk_wf_from_json.argtypes = [ctypes.c_char_p]

    lib.csilk_wf_to_mermaid.restype = ctypes.c_void_p
    lib.csilk_wf_to_mermaid.argtypes = [ctypes.c_void_p]

    lib.csilk_wf_register_monitor.restype = None
    lib.csilk_wf_register_monitor.argtypes = [ctypes.c_void_p, CsilkCtxPtr]

    lib.csilk_wf_set_budget.restype = None
    lib.csilk_wf_set_budget.argtypes = [ctypes.c_void_p, ctypes.c_int]

    # AI Engine Drivers
    lib.csilk_ai_new.restype = ctypes.c_void_p
    lib.csilk_ai_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_ai_chat.restype = ctypes.c_int
    lib.csilk_ai_chat.argtypes = [ctypes.c_void_p, ctypes.POINTER(CsilkAiChatRequest), ctypes.POINTER(CsilkAiChatResponse)]

    lib.csilk_ai_embeddings.restype = ctypes.c_int
    lib.csilk_ai_embeddings.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t, ctypes.POINTER(CsilkAiEmbeddingsResponse)]

    lib.csilk_ai_free.restype = None
    lib.csilk_ai_free.argtypes = [ctypes.c_void_p]

    lib.csilk_ai_chat_response_free.restype = None
    lib.csilk_ai_chat_response_free.argtypes = [ctypes.POINTER(CsilkAiChatResponse)]

    lib.csilk_ai_embeddings_response_free.restype = None
    lib.csilk_ai_embeddings_response_free.argtypes = [ctypes.POINTER(CsilkAiEmbeddingsResponse)]

    lib.csilk_ai_context_new.restype = ctypes.c_void_p
    lib.csilk_ai_context_new.argtypes = [ctypes.c_size_t]

    lib.csilk_ai_context_add.restype = None
    lib.csilk_ai_context_add.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_ai_context_clear.restype = None
    lib.csilk_ai_context_clear.argtypes = [ctypes.c_void_p]

    lib.csilk_ai_context_free.restype = None
    lib.csilk_ai_context_free.argtypes = [ctypes.c_void_p]

    lib.csilk_ai_get_stats.restype = None
    lib.csilk_ai_get_stats.argtypes = [ctypes.POINTER(CsilkAiStats)]

    lib.csilk_ai_stats_to_json.restype = ctypes.c_char_p
    lib.csilk_ai_stats_to_json.argtypes = [ctypes.POINTER(CsilkAiStats)]

    lib.csilk_ai_register_monitor.restype = None
    lib.csilk_ai_register_monitor.argtypes = [ctypes.c_void_p]

    # Database
    lib.csilk_db_init.restype = None
    lib.csilk_db_init.argtypes = []

    lib.csilk_db_pool_new.restype = ctypes.c_void_p
    lib.csilk_db_pool_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_db_pool_free.restype = None
    lib.csilk_db_pool_free.argtypes = [ctypes.c_void_p]

    lib.csilk_db_query_json.restype = ctypes.c_void_p
    lib.csilk_db_query_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_db_query_param_json.restype = ctypes.c_void_p
    lib.csilk_db_query_param_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]

    lib.csilk_db_exec.restype = ctypes.c_int
    lib.csilk_db_exec.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.csilk_db_get_stats.restype = None
    lib.csilk_db_get_stats.argtypes = [ctypes.POINTER(CsilkDbStats)]

    # cJSON
    lib.cJSON_PrintUnformatted.restype = ctypes.c_void_p
    lib.cJSON_PrintUnformatted.argtypes = [ctypes.c_void_p]

    lib.cJSON_Delete.restype = None
    lib.cJSON_Delete.argtypes = [ctypes.c_void_p]

    # Cryptographic utilities
    lib.csilk_crypto_fill_random.restype = ctypes.c_int
    lib.csilk_crypto_fill_random.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    lib.csilk_generate_uuid.restype = None
    lib.csilk_generate_uuid.argtypes = [ctypes.c_char_p]

    lib.csilk_csrf_generate_token.restype = ctypes.c_int
    lib.csilk_csrf_generate_token.argtypes = [ctypes.c_char_p, ctypes.c_size_t]

    # cJSON Parsing
    lib.cJSON_Parse.restype = ctypes.c_void_p
    lib.cJSON_Parse.argtypes = [ctypes.c_char_p]

    # Permission management
    lib.csilk_perm_init.restype = None
    lib.csilk_perm_init.argtypes = []

    lib.csilk_perm_set_default.restype = ctypes.c_int
    lib.csilk_perm_set_default.argtypes = [ctypes.c_char_p]

    lib.csilk_perm_check.restype = ctypes.c_int
    lib.csilk_perm_check.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_perm_require.restype = None
    lib.csilk_perm_require.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_perm_simple_init.restype = None
    lib.csilk_perm_simple_init.argtypes = []

    lib.csilk_perm_simple_allow.restype = ctypes.c_int
    lib.csilk_perm_simple_allow.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_perm_simple_clear.restype = None
    lib.csilk_perm_simple_clear.argtypes = []

    lib.csilk_perm_auto_middleware.restype = None
    lib.csilk_perm_auto_middleware.argtypes = [CsilkCtxPtr]

    # Vector Database
    lib.csilk_vector_db_new.restype = ctypes.c_void_p
    lib.csilk_vector_db_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]

    lib.csilk_vector_db_upsert.restype = ctypes.c_int
    lib.csilk_vector_db_upsert.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(CsilkVectorPoint), ctypes.c_size_t]

    lib.csilk_vector_db_search.restype = ctypes.c_int
    lib.csilk_vector_db_search.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(CsilkVectorSearchResponse)]

    lib.csilk_vector_db_free.restype = None
    lib.csilk_vector_db_free.argtypes = [ctypes.c_void_p]

    lib.csilk_vector_search_response_free.restype = None
    lib.csilk_vector_search_response_free.argtypes = [ctypes.POINTER(CsilkVectorSearchResponse)]

    # OpenTelemetry W3C Tracing
    lib.csilk_trace_middleware.restype = None
    lib.csilk_trace_middleware.argtypes = [CsilkCtxPtr]

    lib.csilk_ctx_get_trace_id.restype = ctypes.c_char_p
    lib.csilk_ctx_get_trace_id.argtypes = [CsilkCtxPtr]

    lib.csilk_ctx_get_span_id.restype = ctypes.c_char_p
    lib.csilk_ctx_get_span_id.argtypes = [CsilkCtxPtr]

    # Circuit Breaker
    lib.csilk_circuit_breaker_new.restype = CsilkCircuitBreakerPtr
    lib.csilk_circuit_breaker_new.argtypes = [CsilkCircuitBreakerConfigPtr]

    lib.csilk_circuit_breaker_middleware.restype = None
    lib.csilk_circuit_breaker_middleware.argtypes = [CsilkCtxPtr, CsilkCircuitBreakerPtr]

    lib.csilk_circuit_breaker_record_success.restype = None
    lib.csilk_circuit_breaker_record_success.argtypes = [CsilkCircuitBreakerPtr]

    lib.csilk_circuit_breaker_record_failure.restype = None
    lib.csilk_circuit_breaker_record_failure.argtypes = [CsilkCircuitBreakerPtr]

    lib.csilk_circuit_breaker_get_state.restype = ctypes.c_int
    lib.csilk_circuit_breaker_get_state.argtypes = [CsilkCircuitBreakerPtr]

    lib.csilk_circuit_breaker_free.restype = None
    lib.csilk_circuit_breaker_free.argtypes = [CsilkCircuitBreakerPtr]

    # Sliding Window Rate Limiter
    lib.csilk_sliding_limiter_new.restype = CsilkSlidingLimiterPtr
    lib.csilk_sliding_limiter_new.argtypes = [ctypes.c_int, ctypes.c_uint64]

    lib.csilk_sliding_rate_limit_middleware.restype = None
    lib.csilk_sliding_rate_limit_middleware.argtypes = [CsilkCtxPtr, CsilkSlidingLimiterPtr]

    lib.csilk_sliding_limiter_free.restype = None
    lib.csilk_sliding_limiter_free.argtypes = [CsilkSlidingLimiterPtr]

    _cached_lib = lib
    return lib

