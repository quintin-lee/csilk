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

class CsilkCorsConfig(ctypes.Structure):
    _fields_ = [
        ("allow_origin", ctypes.c_char_p),
        ("allow_methods", ctypes.c_char_p),
        ("allow_headers", ctypes.c_char_p),
        ("allow_credentials", ctypes.c_int),
        ("max_age", ctypes.c_int)
    ]

CsilkHandler = ctypes.CFUNCTYPE(None, CsilkCtxPtr)
CsilkWsMessageCallback = ctypes.CFUNCTYPE(None, CsilkCtxPtr, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int)
CsilkHeaderCb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p)

_cached_lib = None

def load_lib():
    env_path = os.environ.get("LIBCSILK_PATH")
    if env_path:
        return ctypes.CDLL(env_path)
    
    base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    lib_name = "libcsilk.so" if sys.platform != "darwin" else "libcsilk.dylib"
    build_path = os.path.join(base_dir, "build", lib_name)
    
    if os.path.exists(build_path):
        return ctypes.CDLL(build_path)
    
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
    
    # Server management
    lib.csilk_server_stop.restype = None
    lib.csilk_server_stop.argtypes = [CsilkServerPtr]
    
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
    
    _cached_lib = lib
    return lib
