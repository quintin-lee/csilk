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
        ("recovery_timeout_ms", ctypes.c_int),
        ("half_open_success_threshold", ctypes.c_int),
    ]
CsilkCircuitBreakerConfigPtr = ctypes.POINTER(CsilkCircuitBreakerConfig)

class CsilkAsyncToken(ctypes.Structure):
    _fields_ = [
        ("ctx", CsilkCtxPtr),
        ("request_seq", ctypes.c_uint64),
    ]
CsilkAsyncTokenPtr = ctypes.POINTER(CsilkAsyncToken)

class CsilkSlidingLimiter(ctypes.Structure):
    pass
CsilkSlidingLimiterPtr = ctypes.POINTER(CsilkSlidingLimiter)

class CsilkCorsConfig(ctypes.Structure):
    _fields_ = [
        ("allow_origin", ctypes.c_char_p),
        ("allow_methods", ctypes.c_char_p),
        ("allow_headers", ctypes.c_char_p),
        ("allow_credentials", ctypes.c_int),
        ("max_age", ctypes.c_int),
        ("allow_origin_list", ctypes.POINTER(ctypes.c_char_p)),
        ("echo_origin", ctypes.c_int),
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
        ("max_history_messages", ctypes.c_int),
        ("driver", ctypes.c_char_p),
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
class CsilkAiToolCall(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char_p),
        ("name", ctypes.c_char_p),
        ("arguments", ctypes.c_char_p)
    ]
CsilkAiToolCallPtr = ctypes.POINTER(CsilkAiToolCall)

class CsilkAiMessage(ctypes.Structure):
    _fields_ = [
        ("role", ctypes.c_char_p),
        ("content", ctypes.c_char_p),
        ("tool_calls", CsilkAiToolCallPtr),
        ("tool_call_count", ctypes.c_size_t),
        ("tool_call_id", ctypes.c_char_p),
    ]

class CsilkAiChatResponse(ctypes.Structure):
    _fields_ = [
        ("content", ctypes.c_char_p),
        ("reasoning_content", ctypes.c_char_p),
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
        ("on_reasoning_chunk", ctypes.c_void_p),
        ("user_data", ctypes.c_void_p),
        ("timeout_ms", ctypes.c_int),
        ("tools", CsilkAiToolPtr),
        ("tool_count", ctypes.c_size_t),
        ("tool_choice", ctypes.c_char_p),
        ("reasoning_effort", ctypes.c_char_p),
        ("has_enable_thinking", ctypes.c_bool),
        ("enable_thinking", ctypes.c_bool),
        ("chat_template_kwargs", ctypes.c_void_p)
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
CsilkDispatchCb = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

CsilkWfHandler = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, CsilkDataPtr, ctypes.c_void_p)
CsilkWfRouter = ctypes.CFUNCTYPE(ctypes.c_void_p, CsilkDataPtr)
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
        ("enable_uring_sqpoll", ctypes.c_int),
        ("backpressure_max_queue_depth", ctypes.c_size_t),
        ("backpressure_max_latency_us", ctypes.c_uint),

        ("enable_openapi", ctypes.c_int)
    ]

_cached_lib = None

if sys.platform.startswith("win"):
    _TEST_SUPPORT_SUFFIX = ".dll"
elif sys.platform == "darwin":
    _TEST_SUPPORT_SUFFIX = ".dylib"
else:
    _TEST_SUPPORT_SUFFIX = ".so"
_TEST_SUPPORT_NAMES = (
    "libcsilk_test_support_shared" + _TEST_SUPPORT_SUFFIX,
    "libcsilk-test-support" + _TEST_SUPPORT_SUFFIX,
)

def _attach_test_support(lib, search_dirs):
    """Bind csilk_test_ctx_* from a test-support library, when one is present.

    Search order: LIBCSILK_TEST_SUPPORT_PATH (file or directory), then every
    directory beside the libraries we already know about.  Loading a separate
    support library keeps production installs test-free while still enabling
    Context.create_test_context() from a build tree.
    """
    support_path = os.environ.get("LIBCSILK_TEST_SUPPORT_PATH")
    candidates = []
    if support_path:
        if os.path.isdir(support_path):
            candidates += [os.path.join(support_path, n) for n in _TEST_SUPPORT_NAMES]
        else:
            candidates.append(support_path)
    for base in search_dirs:
        candidates += [os.path.join(base, n) for n in _TEST_SUPPORT_NAMES]

    for cand in candidates:
        if not cand or cand.endswith(".a") or not os.path.exists(cand):
            continue
        try:
            support = ctypes.CDLL(cand, mode=ctypes.RTLD_GLOBAL)
        except OSError:
            continue
        lib.csilk_test_ctx_new = support.csilk_test_ctx_new
        lib.csilk_test_ctx_new.restype = CsilkCtxPtr
        lib.csilk_test_ctx_new.argtypes = []
        lib.csilk_test_ctx_free = support.csilk_test_ctx_free
        lib.csilk_test_ctx_free.restype = None
        lib.csilk_test_ctx_free.argtypes = [CsilkCtxPtr]
        lib._csilk_test_support = support
        return True
    return False

def load_lib():
    env_path = os.environ.get("LIBCSILK_PATH")
    lib = None
    search_dirs = []
    if env_path:
        lib = ctypes.CDLL(env_path)
        search_dirs.append(os.path.dirname(env_path))
    else:
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
            lib = ctypes.CDLL(pkg_path)
            search_dirs.append(pkg_dir)
        else:
            # 2. Look in the build directory (for local dev)
            base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
            build_path = os.path.join(base_dir, "build", lib_name)
            if os.path.exists(build_path):
                lib = ctypes.CDLL(build_path)
                search_dirs.append(os.path.dirname(build_path))
            else:
                # 3. Fallback to system paths
                try:
                    lib = ctypes.CDLL(lib_name)
                except OSError as e:
                    raise OSError(f"Could not load {lib_name}. Please compile csilk as shared library and configure LIBCSILK_PATH.") from e

    _attach_test_support(lib, search_dirs)
    return lib

def has_symbol(lib, name: str) -> bool:
    """Return True if ``lib`` exports or binds the C symbol ``name``.

    Args:
        lib: Loaded csilk shared library handle (e.g., from ``get_bindings()``).
        name: C symbol name to probe (e.g., ``"csilk_ai_new"``).
    """
    return getattr(lib, name, None) is not None


def require_symbol(lib, name: str, feature: str):
    """Return ``lib.<name>`` or raise ``NotImplementedError`` if absent.

    Args:
        lib: Loaded csilk shared library handle (e.g., from ``get_bindings()``).
        name: Required C symbol name (e.g., ``"csilk_ai_new"``).
        feature: Feature or CMake build flag name (e.g., ``"CSILK_ENABLE_AI"``).

    Raises:
        NotImplementedError: When ``lib`` was built without ``feature`` support.
    """
    if not has_symbol(lib, name):
        raise NotImplementedError(
            f"csilk built without {feature} support (missing {name})"
        )
    return getattr(lib, name)


def _bind(lib, name: str, restype, argtypes, optional: bool = False) -> bool:
    """Bind ``restype`` and ``argtypes`` on ``lib.<name>``.

    Core symbols (``optional=False``) raise ``AttributeError`` immediately when
    missing. Optional symbols are bound only when present in ``lib``.
    """
    if optional and not has_symbol(lib, name):
        return False
    fn = getattr(lib, name)
    fn.restype = restype
    fn.argtypes = argtypes
    return True


_CORE_SYMBOL_SPECS = (
    # App management
    ("csilk_app_new", CsilkAppPtr, [ctypes.c_char_p]),
    ("csilk_app_free", None, [CsilkAppPtr]),
    ("csilk_app_add_route", None, [CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler]),
    ("csilk_app_static", None, [CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p]),
    ("csilk_app_enable_openapi", None, [CsilkAppPtr, ctypes.c_int]),
    ("csilk_app_set_server_config", None, [CsilkAppPtr, CsilkServerConfig]),
    # Logger
    ("csilk_app_log_level", None, [CsilkAppPtr, ctypes.c_int]),
    ("csilk_app_log_file", None, [CsilkAppPtr, ctypes.c_char_p, ctypes.c_size_t]),
    ("csilk_app_log_json", None, [CsilkAppPtr, ctypes.c_int]),
    # Middleware application
    ("csilk_app_use", None, [CsilkAppPtr, CsilkHandler]),
    ("csilk_app_use_group", None, [CsilkAppPtr, ctypes.c_char_p, CsilkHandler]),
    ("csilk_app_apply_config", None, [CsilkAppPtr]),
    ("csilk_app_run", ctypes.c_int, [CsilkAppPtr, ctypes.c_int]),
    ("csilk_app_server", CsilkServerPtr, [CsilkAppPtr]),
    ("csilk_app_router", ctypes.c_void_p, [CsilkAppPtr]),
    # Server management
    ("csilk_server_stop", None, [CsilkServerPtr]),
    # Route Groups
    ("csilk_group_new", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
    ("csilk_group_group", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
    ("csilk_group_use", None, [ctypes.c_void_p, CsilkHandler]),
    (
        "csilk_group_add_route_extended_perm",
        None,
        [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_char_p, ctypes.c_char_p,
        ],
    ),
    ("csilk_group_free", None, [ctypes.c_void_p]),
    # Context getters
    ("csilk_get_method", ctypes.c_char_p, [CsilkCtxPtr]),
    ("csilk_get_path", ctypes.c_char_p, [CsilkCtxPtr]),
    ("csilk_get_body", ctypes.c_void_p, [CsilkCtxPtr, ctypes.POINTER(ctypes.c_size_t)]),
    ("csilk_get_body_len", ctypes.c_size_t, [CsilkCtxPtr]),
    ("csilk_get_query", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_get_param", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_char_p]),
    # Context path parameters iterator
    ("csilk_get_params_count", ctypes.c_int, [CsilkCtxPtr]),
    ("csilk_get_param_key", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_get_param_value", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_int]),
    # Context states
    ("csilk_is_websocket", ctypes.c_int, [CsilkCtxPtr]),
    ("csilk_is_sse", ctypes.c_int, [CsilkCtxPtr]),
    ("csilk_is_aborted", ctypes.c_int, [CsilkCtxPtr]),
    ("csilk_get_header", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_get_response_header", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_get_request_id", ctypes.c_char_p, [CsilkCtxPtr]),
    ("csilk_set_request_id", None, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_set_request_header", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    ("csilk_get_status", ctypes.c_int, [CsilkCtxPtr]),
    ("csilk_set_status", None, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_ctx_set_websocket", None, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_ctx_set_sse", None, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_ctx_set_async", None, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_ctx_acquire_async", CsilkAsyncToken, [CsilkCtxPtr]),
    ("csilk_ctx_release_async", None, [ctypes.POINTER(CsilkAsyncToken)]),
    ("csilk_async_token_validate", ctypes.c_int, [ctypes.POINTER(CsilkAsyncToken)]),
    ("csilk_is_async", ctypes.c_int, [CsilkCtxPtr]),
    ("csilk_dispatch", None, [CsilkCtxPtr, CsilkDispatchCb, ctypes.c_void_p]),
    # Response helpers
    ("csilk_status", None, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_string", None, [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]),
    ("csilk_set_header", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    ("csilk_file", None, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_redirect", None, [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]),
    ("csilk_add_header", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    ("csilk_json_error", None, [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]),
    ("csilk_redirect_simple", None, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_response_write", ctypes.c_int32, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_size_t]),
    ("csilk_response_get_write_queue_size", ctypes.c_size_t, [CsilkCtxPtr]),
    ("csilk_response_is_writable", ctypes.c_int32, [CsilkCtxPtr]),
    ("csilk_response_set_watermarks", None, [CsilkCtxPtr, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t]),
    ("csilk_response_end", None, [CsilkCtxPtr]),
    ("csilk_push_promise", ctypes.c_int32, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    # Middleware flow
    ("csilk_next", None, [CsilkCtxPtr]),
    ("csilk_abort", None, [CsilkCtxPtr]),
    # WebSockets
    ("csilk_ws_handshake", None, [CsilkCtxPtr]),
    ("csilk_ws_send", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]),
    ("csilk_ws_close", None, [CsilkCtxPtr, ctypes.c_uint16, ctypes.c_char_p]),
    ("csilk_ws_join_room", None, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_ws_leave_room", None, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_ws_broadcast_room", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    # SSE
    ("csilk_sse_init", None, [CsilkCtxPtr]),
    ("csilk_sse_send", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    ("csilk_sse_close", None, [CsilkCtxPtr]),
    # WebSocket callbacks
    ("csilk_set_on_ws_message", None, [CsilkCtxPtr, CsilkWsMessageCallback]),
    ("csilk_get_on_ws_message", ctypes.c_void_p, [CsilkCtxPtr]),
    # Server hooks
    ("csilk_server_add_hook", None, [CsilkServerPtr, ctypes.c_int, ctypes.c_void_p]),
    ("csilk_server_set_not_found_handler", None, [CsilkServerPtr, CsilkHandler]),
    ("csilk_server_set_spa_fallback", None, [CsilkServerPtr, ctypes.c_char_p]),
    ("csilk_server_get_stats", None, [CsilkServerPtr, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]),
    ("csilk_server_set_max_connections", ctypes.c_int, [CsilkServerPtr, ctypes.c_int]),
    # Extended routing with metadata
    (
        "csilk_app_add_route_extended",
        None,
        [
            CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
        ],
    ),
    (
        "csilk_app_add_route_perm",
        None,
        [
            CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
            ctypes.c_char_p, ctypes.c_char_p,
        ],
    ),
    (
        "csilk_app_add_route_extended_perm",
        None,
        [
            CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_char_p, ctypes.c_char_p,
        ],
    ),
    # Utilities
    ("csilk_free", None, [ctypes.c_void_p]),
    # Cookies & Forms
    ("csilk_get_cookie", ctypes.c_char_p, [CsilkCtxPtr, ctypes.c_char_p]),
    (
        "csilk_set_cookie",
        None,
        [
            CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
        ],
    ),
    ("csilk_parse_form_urlencoded", None, [CsilkCtxPtr]),
    # Hash Map Iteration
    ("csilk_for_each_header", None, [CsilkCtxPtr, CsilkHeaderCb, ctypes.c_void_p]),
    ("csilk_for_each_query", None, [CsilkCtxPtr, CsilkHeaderCb, ctypes.c_void_p]),
    ("csilk_for_each_form_field", None, [CsilkCtxPtr, CsilkHeaderCb, ctypes.c_void_p]),
    # Context Handler Info
    ("csilk_ctx_get_handler_path", ctypes.c_char_p, [CsilkCtxPtr]),
    ("csilk_ctx_get_handler_perm_required", ctypes.c_char_p, [CsilkCtxPtr]),
    ("csilk_ctx_get_handler_perm_resource", ctypes.c_char_p, [CsilkCtxPtr]),
    # Request-local context storage (opaque pointers)
    ("csilk_set", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_void_p]),
    ("csilk_get", ctypes.c_void_p, [CsilkCtxPtr, ctypes.c_char_p]),
    # Storage functions
    ("csilk_set_string", ctypes.c_int, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]),
    ("csilk_get_string", ctypes.c_void_p, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_incr", ctypes.c_longlong, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_int]),
    # Response body mutation
    ("csilk_set_response_body", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]),
    ("csilk_get_response_body", ctypes.c_char_p, [CsilkCtxPtr, ctypes.POINTER(ctypes.c_size_t)]),
    # Built-in C Middlewares
    ("csilk_recovery_handler", None, [CsilkCtxPtr]),
    ("csilk_logger_handler", None, [CsilkCtxPtr]),
    ("csilk_waf_middleware", None, [CsilkCtxPtr]),
    ("csilk_request_id_middleware", None, [CsilkCtxPtr]),
    ("csilk_health_check_handler", None, [CsilkCtxPtr]),
    ("csilk_ready_check_handler", None, [CsilkCtxPtr]),
    ("csilk_rate_limit_middleware", None, [CsilkCtxPtr, ctypes.c_int]),
    ("csilk_csrf_middleware", None, [CsilkCtxPtr]),
    ("csilk_gzip_middleware", None, [CsilkCtxPtr]),
    ("csilk_cors_middleware", None, [CsilkCtxPtr, ctypes.POINTER(CsilkCorsConfig)]),
    ("csilk_multipart_parse", None, [CsilkCtxPtr, CsilkMultipartHandler]),
    # JWT Middlewares & Helpers
    ("csilk_jwt_middleware", None, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_ctx_get_jwt_payload_json", ctypes.c_void_p, [CsilkCtxPtr]),
    ("csilk_ctx_cleanup_jwt_payload", None, [CsilkCtxPtr]),
    ("csilk_jwt_generate_json", ctypes.c_void_p, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
    # Session Management
    ("csilk_session_init", None, []),
    ("csilk_session_start", None, [CsilkCtxPtr]),
    ("csilk_session_set", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_void_p]),
    ("csilk_session_get", ctypes.c_void_p, [CsilkCtxPtr, ctypes.c_char_p]),
    ("csilk_session_destroy", None, [CsilkCtxPtr]),
    ("csilk_session_get_id", ctypes.c_char_p, [CsilkCtxPtr]),
    # Utilities (heap/string)
    ("csilk_malloc", ctypes.c_void_p, [ctypes.c_size_t]),
    ("csilk_strdup", ctypes.c_void_p, [ctypes.c_char_p]),
    # csilk_json serialization, parsing & free
    ("csilk_json_serialize", ctypes.c_void_p, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]),
    ("csilk_json_free", None, [ctypes.c_void_p]),
    ("csilk_json_parse", ctypes.c_void_p, [ctypes.c_char_p]),
    # Cryptographic utilities
    ("csilk_crypto_fill_random", ctypes.c_int, [ctypes.c_void_p, ctypes.c_size_t]),
    ("csilk_generate_uuid", None, [ctypes.c_char_p]),
    ("csilk_csrf_generate_token", ctypes.c_int, [ctypes.c_char_p, ctypes.c_size_t]),
    # Circuit Breaker
    ("csilk_circuit_breaker_new", CsilkCircuitBreakerPtr, [CsilkCircuitBreakerConfigPtr]),
    ("csilk_circuit_breaker_middleware", None, [CsilkCtxPtr, CsilkCircuitBreakerPtr]),
    ("csilk_circuit_breaker_record_success", None, [CsilkCircuitBreakerPtr]),
    ("csilk_circuit_breaker_record_failure", None, [CsilkCircuitBreakerPtr]),
    ("csilk_circuit_breaker_get_state", ctypes.c_int, [CsilkCircuitBreakerPtr]),
    ("csilk_circuit_breaker_free", None, [CsilkCircuitBreakerPtr]),
    # Sliding Window Rate Limiter
    ("csilk_sliding_limiter_new", CsilkSlidingLimiterPtr, [ctypes.c_int, ctypes.c_uint64]),
    ("csilk_sliding_rate_limit_middleware", None, [CsilkCtxPtr, CsilkSlidingLimiterPtr]),
    ("csilk_sliding_limiter_free", None, [CsilkSlidingLimiterPtr]),
)

# Test utilities are optional: production shared libraries intentionally
# do not export the test-only context helpers.
_TEST_SUPPORT_SYMBOL_SPECS = (
    ("csilk_test_ctx_new", CsilkCtxPtr, []),
    ("csilk_test_ctx_free", None, [CsilkCtxPtr]),
)

# Optional module symbol bindings (CSILK_ENABLE_* / CSILK_BUILD_FULL=ON).
_OPTIONAL_SYMBOL_GROUPS = {
    "db": (
        ("csilk_db_init", None, []),
        ("csilk_db_pool_new", ctypes.c_void_p, [ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_db_pool_free", None, [ctypes.c_void_p]),
        ("csilk_db_query_json", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_db_query_param_json", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]),
        ("csilk_db_exec", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_db_get_stats", None, [ctypes.POINTER(CsilkDbStats)]),
    ),
    "mq": (
        ("csilk_server_get_mq", ctypes.c_void_p, [CsilkServerPtr]),
        ("csilk_mq_use", None, [ctypes.c_void_p, ctypes.c_char_p, CsilkMqHandler]),
        ("csilk_mq_subscribe", None, [ctypes.c_void_p, ctypes.c_char_p, CsilkMqHandler]),
        ("csilk_mq_publish", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]),
        ("csilk_mq_next", None, [ctypes.c_void_p]),
        ("csilk_mq_abort", None, [ctypes.c_void_p]),
        ("csilk_mq_get_topic", ctypes.c_char_p, [ctypes.c_void_p]),
        ("csilk_mq_get_payload", ctypes.c_void_p, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]),
        ("csilk_mq_get_stats", None, [ctypes.c_void_p, ctypes.POINTER(CsilkMqStats)]),
        ("csilk_mq_set_persistence", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p]),
    ),
    "ai": (
        ("csilk_ai_new", ctypes.c_void_p, [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_ai_chat", ctypes.c_int, [ctypes.c_void_p, ctypes.POINTER(CsilkAiChatRequest), ctypes.POINTER(CsilkAiChatResponse)]),
        ("csilk_ai_embeddings", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t, ctypes.POINTER(CsilkAiEmbeddingsResponse)]),
        ("csilk_ai_free", None, [ctypes.c_void_p]),
        ("csilk_ai_chat_response_free", None, [ctypes.POINTER(CsilkAiChatResponse)]),
        ("csilk_ai_embeddings_response_free", None, [ctypes.POINTER(CsilkAiEmbeddingsResponse)]),
        ("csilk_ai_context_new", ctypes.c_void_p, [ctypes.c_size_t]),
        ("csilk_ai_context_add", None, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_ai_context_clear", None, [ctypes.c_void_p]),
        ("csilk_ai_context_free", None, [ctypes.c_void_p]),
        ("csilk_ai_get_stats", None, [ctypes.POINTER(CsilkAiStats)]),
        ("csilk_ai_stats_to_json", ctypes.c_char_p, [ctypes.POINTER(CsilkAiStats)]),
        ("csilk_ai_register_monitor", None, [ctypes.c_void_p]),
    ),
    "vector": (
        ("csilk_vector_db_new", ctypes.c_void_p, [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_vector_db_upsert", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(CsilkVectorPoint), ctypes.c_size_t]),
        ("csilk_vector_db_search", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(CsilkVectorSearchResponse)]),
        ("csilk_vector_db_free", None, [ctypes.c_void_p]),
        ("csilk_vector_db_new_embedded_mmap", ctypes.c_void_p, [ctypes.c_char_p]),
        ("csilk_vector_db_save_embedded", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_vector_search_response_free", None, [ctypes.POINTER(CsilkVectorSearchResponse)]),
    ),
    "mcp": (),
    "workflow": (
        ("csilk_wf_serve_ui", None, [CsilkAppPtr, ctypes.c_char_p]),
        ("csilk_wf_new", ctypes.c_void_p, [ctypes.c_char_p]),
        ("csilk_wf_free", None, [ctypes.c_void_p]),
        ("csilk_wf_add", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_add_ai", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_add_vector_search", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_register_tool", None, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_set_tool_discovery", None, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_get_node", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_wf_node_set_entry", None, [ctypes.c_void_p, ctypes.c_int]),
        ("csilk_wf_bind", None, [ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_on", None, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_on_loop", None, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_on_error", None, [ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_route", None, [ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_node_set_join", None, [ctypes.c_void_p, ctypes.c_int]),
        ("csilk_wf_node_set_interactive", None, [ctypes.c_void_p, ctypes.c_int]),
        ("csilk_wf_node_set_schema", None, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_wf_signal_continue", None, [ctypes.c_void_p, ctypes.c_char_p, CsilkDataPtr, ctypes.c_void_p]),
        ("csilk_wf_node_set_timeout", None, [ctypes.c_void_p, ctypes.c_int]),
        ("csilk_wf_set_ttl", None, [ctypes.c_void_p, ctypes.c_int]),
        ("csilk_wf_node_set_retry", None, [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]),
        ("csilk_wf_node_set_remote", None, [ctypes.c_void_p, ctypes.c_int]),
        ("csilk_wf_enable_distributed", None, [ctypes.c_void_p, ctypes.c_void_p]),
        ("csilk_wf_set_persistence", None, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_wf_run", ctypes.c_char_p, [ctypes.c_void_p, CsilkDataPtr, ctypes.c_void_p]),
        ("csilk_wf_resume", None, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_run_traced", None, [ctypes.c_void_p, CsilkDataPtr, ctypes.c_void_p]),
        ("csilk_wf_trace_to_json", ctypes.c_void_p, [ctypes.c_void_p]),
        ("csilk_wf_trace_free", None, [ctypes.c_void_p]),
        # Memory Helpers (Arena-backed)
        ("csilk_wf_data_new", CsilkDataPtr, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_strdup", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csilk_wf_alloc", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_size_t]),
        # Declarative API
        ("csilk_wf_register_handler", None, [ctypes.c_char_p, ctypes.c_void_p]),
        ("csilk_wf_load_yaml", ctypes.c_void_p, [ctypes.c_char_p]),
        ("csilk_wf_from_json", ctypes.c_void_p, [ctypes.c_char_p]),
        ("csilk_wf_to_mermaid", ctypes.c_void_p, [ctypes.c_void_p]),
        ("csilk_wf_register_monitor", None, [ctypes.c_void_p, CsilkCtxPtr]),
        ("csilk_wf_set_budget", None, [ctypes.c_void_p, ctypes.c_int]),
    ),
    "perm": (
        ("csilk_perm_init", None, []),
        ("csilk_perm_set_default", ctypes.c_int, [ctypes.c_char_p]),
        ("csilk_perm_check", ctypes.c_int, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_perm_require", None, [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_perm_simple_init", None, []),
        ("csilk_perm_simple_allow", ctypes.c_int, [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]),
        ("csilk_perm_simple_clear", None, []),
        ("csilk_perm_auto_middleware", None, [CsilkCtxPtr]),
    ),
    "observability": (
        ("csilk_admin_serve", None, [CsilkAppPtr, ctypes.c_char_p]),
        ("csilk_admin_serve_secure", None, [CsilkAppPtr, ctypes.c_char_p, CsilkHandler]),
        ("csilk_trace_middleware", None, [CsilkCtxPtr]),
        ("csilk_ctx_get_trace_id", ctypes.c_char_p, [CsilkCtxPtr]),
        ("csilk_ctx_get_span_id", ctypes.c_char_p, [CsilkCtxPtr]),
    ),
    "grpc": (),
    "wasm": (),
    "http3": (),
}


def get_bindings():
    global _cached_lib
    if _cached_lib is not None:
        return _cached_lib

    lib = load_lib()

    for name, restype, argtypes in _CORE_SYMBOL_SPECS:
        _bind(lib, name, restype, argtypes, optional=False)

    for name, restype, argtypes in _TEST_SUPPORT_SYMBOL_SPECS:
        _bind(lib, name, restype, argtypes, optional=True)

    for specs in _OPTIONAL_SYMBOL_GROUPS.values():
        for name, restype, argtypes in specs:
            _bind(lib, name, restype, argtypes, optional=True)

    _cached_lib = lib
    return lib

