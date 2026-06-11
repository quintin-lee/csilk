import ctypes
from csilk.lib import get_bindings, CsilkHandler, CsilkCtxPtr
from csilk.context import Context

class App:
    def __init__(self, config_path=None):
        self._lib = get_bindings()
        c_config = config_path.encode('utf-8') if config_path else None
        self._app = self._lib.csilk_app_new(c_config)
        if not self._app:
            raise RuntimeError("Failed to create csilk application instance")
        self._handlers = [] # Reference storage to prevent garbage collection of callbacks
        self._websocket_contexts = {}
        self._lib.csilk_session_init()

        # Set up a hook on the server to clean up closed WebSocket contexts
        @ctypes.CFUNCTYPE(None, CsilkCtxPtr)
        def on_conn_close(ctx_ptr):
            ptr_val = ctypes.addressof(ctx_ptr.contents)
            self._websocket_contexts.pop(ptr_val, None)

        self._on_conn_close_cb = on_conn_close # Keep reference to prevent GC
        server = self._lib.csilk_app_server(self._app)
        if server:
            # CSILK_HOOK_CONN_CLOSE is enum value 3
            self._lib.csilk_server_add_hook(server, 3, on_conn_close)

    def free(self):
        if self._app:
            self._lib.csilk_app_free(self._app)
            self._app = None
        self._handlers.clear()
        self._websocket_contexts.clear()

    def route(self, method, path, handler, input_type=None, output_type=None, summary=None, description=None, perm_required=None, perm_resource=None):
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            ctx._register_ws = lambda: self._websocket_contexts.update({ctypes.addressof(ctx_ptr.contents): ctx})
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
                ctx.string(500, f"Internal Server Error: {str(e)}")

            if ctx.is_websocket:
                self._websocket_contexts[ctypes.addressof(ctx_ptr.contents)] = ctx

        self._handlers.append(wrapper)
        self._lib.csilk_app_add_route_extended_perm(
            self._app,
            method.encode('utf-8'),
            path.encode('utf-8'),
            wrapper,
            input_type.encode('utf-8') if input_type else None,
            output_type.encode('utf-8') if output_type else None,
            summary.encode('utf-8') if summary else None,
            description.encode('utf-8') if description else None,
            perm_required.encode('utf-8') if perm_required else None,
            perm_resource.encode('utf-8') if perm_resource else None
        )

    # HTTP method decorators / shortcuts
    def get(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("GET", path, h, **kwargs)
                return h
            return decorator
        self.route("GET", path, handler, **kwargs)

    def post(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("POST", path, h, **kwargs)
                return h
            return decorator
        self.route("POST", path, handler, **kwargs)

    def put(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("PUT", path, h, **kwargs)
                return h
            return decorator
        self.route("PUT", path, handler, **kwargs)

    def delete(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("DELETE", path, h, **kwargs)
                return h
            return decorator
        self.route("DELETE", path, handler, **kwargs)

    def patch(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("PATCH", path, h, **kwargs)
                return h
            return decorator
        self.route("PATCH", path, handler, **kwargs)

    def options(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("OPTIONS", path, h, **kwargs)
                return h
            return decorator
        self.route("OPTIONS", path, handler, **kwargs)

    def head(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("HEAD", path, h, **kwargs)
                return h
            return decorator
        self.route("HEAD", path, handler, **kwargs)

    # Logging config
    def log_level(self, level):
        """Sets the log level: 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR, 5=FATAL"""
        self._lib.csilk_app_log_level(self._app, int(level))

    def log_file(self, path, max_sz=0):
        self._lib.csilk_app_log_file(
            self._app, 
            path.encode('utf-8') if path else None, 
            max_sz
        )

    def log_json(self, enable=True):
        self._lib.csilk_app_log_json(self._app, 1 if enable else 0)

    # Static files
    def static(self, prefix, root_dir):
        self._lib.csilk_app_static(
            self._app, 
            prefix.encode('utf-8'), 
            root_dir.encode('utf-8')
        )

    # OpenAPI
    def enable_openapi(self, enable=True):
        self._lib.csilk_app_enable_openapi(self._app, 1 if enable else 0)

    # Middleware registration
    def use(self, handler):
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
                ctx.string(500, f"Internal Server Error: {str(e)}")

        self._handlers.append(wrapper)
        self._lib.csilk_app_use(self._app, wrapper)

    def use_group(self, prefix, handler):
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
                ctx.string(500, f"Internal Server Error: {str(e)}")

        self._handlers.append(wrapper)
        self._lib.csilk_app_use_group(self._app, prefix.encode('utf-8'), wrapper)

    def apply_config(self):
        self._lib.csilk_app_apply_config(self._app)

    # Run / Stop
    def run(self, port):
        return self._lib.csilk_app_run(self._app, port)

    def stop(self):
        server = self._lib.csilk_app_server(self._app)
        if server:
            self._lib.csilk_server_stop(server)

    def __del__(self):
        self.free()

# Built-in C middlewares wrappers
def recovery_middleware(ctx):
    get_bindings().csilk_recovery_handler(ctx._ctx)

def logger_middleware(ctx):
    get_bindings().csilk_logger_handler(ctx._ctx)

def waf_middleware(ctx):
    get_bindings().csilk_waf_middleware(ctx._ctx)

def request_id_middleware(ctx):
    get_bindings().csilk_request_id_middleware(ctx._ctx)

def health_check_handler(ctx):
    get_bindings().csilk_health_check_handler(ctx._ctx)

def ready_check_handler(ctx):
    get_bindings().csilk_ready_check_handler(ctx._ctx)

def csrf_middleware(ctx):
    get_bindings().csilk_csrf_middleware(ctx._ctx)

def gzip_middleware(ctx):
    get_bindings().csilk_gzip_middleware(ctx._ctx)

def rate_limit(limit):
    def middleware(ctx):
        get_bindings().csilk_rate_limit_middleware(ctx._ctx, limit)
    return middleware

def cors(allow_origin="*", allow_methods="GET, POST, PUT, DELETE, OPTIONS", allow_headers="Content-Type, Authorization", allow_credentials=False, max_age=86400):
    from csilk.lib import CsilkCorsConfig
    config = CsilkCorsConfig(
        allow_origin=allow_origin.encode('utf-8') if allow_origin else None,
        allow_methods=allow_methods.encode('utf-8') if allow_methods else None,
        allow_headers=allow_headers.encode('utf-8') if allow_headers else None,
        allow_credentials=1 if allow_credentials else 0,
        max_age=max_age
    )
    def middleware(ctx):
        get_bindings().csilk_cors_middleware(ctx._ctx, ctypes.byref(config))
    return middleware
