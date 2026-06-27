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
        self._groups = []
        self._hooks = []
        self._websocket_contexts = {}
        self._exception_handlers = {}
        self._lib.csilk_session_init()

        import asyncio
        import threading
        self._loop = asyncio.new_event_loop()
        def _run_loop():
            asyncio.set_event_loop(self._loop)
            self._loop.run_forever()
        self._loop_thread = threading.Thread(target=_run_loop, daemon=True)
        self._loop_thread.start()

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
        for g in self._groups:
            g.free()
        self._groups.clear()
        self._hooks.clear()
        self._websocket_contexts.clear()
        self._exception_handlers.clear()
        if hasattr(self, '_loop'):
            self._loop.call_soon_threadsafe(self._loop.stop)

    def exception_handler(self, exc_class):
        """Decorator to register a global exception handler for a specific exception class."""
        def decorator(handler):
            self._exception_handlers[exc_class] = handler
            return handler
        return decorator

    def _handle_exception(self, ctx, exc):
        for exc_class, handler in self._exception_handlers.items():
            if isinstance(exc, exc_class):
                try:
                    handler(ctx, exc)
                    return True
                except Exception as inner_e:
                    import traceback
                    traceback.print_exc()
                    ctx.string(500, f"Internal Server Error (Error in exception handler): {str(inner_e)}")
                    return True
        return False

    def route(self, method, path, handler, input_type=None, output_type=None, summary=None, description=None, perm_required=None, perm_resource=None):
        import asyncio
        import weakref
        is_coro = asyncio.iscoroutinefunction(handler)
        app_ref = weakref.proxy(self._app)

        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            ctx._register_ws = lambda: app_ref._websocket_contexts.update({ctypes.addressof(ctx_ptr.contents): ctx})
            
            if is_coro:
                ctx.dispatch_async(handler, app_ref._loop)
            else:
                try:
                    handler(ctx)
                except Exception as e:
                    if not app_ref._handle_exception(ctx, e):
                        import traceback
                        traceback.print_exc()
                        ctx.string(500, f"Internal Server Error: {str(e)}")
                finally:
                    self._lib.csilk_ctx_cleanup_jwt_payload(ctx_ptr)

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

    def mount_asgi(self, path: str, asgi_app):
        """Mounts an ASGI application (like FastAPI or Starlette) under a wildcard route.
        The path should ideally end with /*path to catch all sub-routes.
        Example: app.mount_asgi("/*path", fastapi_app)
        """
        from .asgi import ASGIAdapter
        adapter = ASGIAdapter(asgi_app)
        self.route("GET", path, adapter)
        self.route("POST", path, adapter)
        self.route("PUT", path, adapter)
        self.route("DELETE", path, adapter)
        self.route("PATCH", path, adapter)
        self.route("OPTIONS", path, adapter)

    # HTTP method decorators / shortcuts
    def get(self, path, handler=None, **kwargs):
        if handler is None:
            def decorator(h):
                self.route("GET", path, h, **kwargs)
                return h
            return decorator
        self.route("GET", path, handler, **kwargs)

    def ws(self, path, ws_class=None, **kwargs):
        """Register a class-based WebSocket handler."""
        def decorator(cls):
            def handler(ctx):
                instance = cls()
                ctx.ws_handshake()
                if hasattr(instance, "on_connect"):
                    instance.on_connect(ctx)
                if hasattr(instance, "on_message"):
                    def on_msg(c, msg, op):
                        instance.on_message(c, msg, op)
                    ctx.set_on_ws_message(on_msg)
            self.route("GET", path, handler, **kwargs)
            return cls
        if ws_class is None:
            return decorator
        return decorator(ws_class)

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
        import weakref
        app_ref = weakref.proxy(self)
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                if not app_ref._handle_exception(ctx, e):
                    import traceback
                    traceback.print_exc()
                    ctx.string(500, f"Internal Server Error: {str(e)}")
            finally:
                self._lib.csilk_ctx_cleanup_jwt_payload(ctx_ptr)

        self._handlers.append(wrapper)
        self._lib.csilk_app_use(self._app, wrapper)

    def use_group(self, prefix, handler):
        import weakref
        app_ref = weakref.proxy(self)
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                if not app_ref._handle_exception(ctx, e):
                    import traceback
                    traceback.print_exc()
                    ctx.string(500, f"Internal Server Error: {str(e)}")
            finally:
                self._lib.csilk_ctx_cleanup_jwt_payload(ctx_ptr)

        self._handlers.append(wrapper)
        self._lib.csilk_app_use_group(self._app, prefix.encode('utf-8'), wrapper)


    def apply_config(self):
        self._lib.csilk_app_apply_config(self._app)

    def set_server_config(self, 
                          idle_timeout_ms=0, 
                          read_timeout_ms=0, 
                          write_timeout_ms=0, 
                          request_timeout_ms=0, 
                          max_body_size=0, 
                          max_header_size=0, 
                          max_url_size=0, 
                          max_headers_count=0, 
                          max_connections=0, 
                          listen_backlog=0, 
                          tcp_nodelay=0, 
                          tcp_keepalive=0, 
                          worker_threads=0, 
                          enable_tls=0, 
                          tls_cert_file=None, 
                          tls_key_file=None, 
                          tls_ca_file=None, 
                          tls_verify_peer=0, 
                          h2_push_enable=0, 
                          h2_max_push_per_request=0, 
                          enable_simd=0, 
                          enable_arena_alignment=0, 
                          enable_openapi=0):
        from csilk.lib import CsilkServerConfig
        config = CsilkServerConfig(
            idle_timeout_ms=idle_timeout_ms,
            read_timeout_ms=read_timeout_ms,
            write_timeout_ms=write_timeout_ms,
            request_timeout_ms=request_timeout_ms,
            max_body_size=max_body_size,
            max_header_size=max_header_size,
            max_url_size=max_url_size,
            max_headers_count=max_headers_count,
            max_connections=max_connections,
            listen_backlog=listen_backlog,
            tcp_nodelay=tcp_nodelay,
            tcp_keepalive=tcp_keepalive,
            worker_threads=worker_threads,
            enable_tls=enable_tls,
            tls_cert_file=tls_cert_file.encode('utf-8') if tls_cert_file else None,
            tls_key_file=tls_key_file.encode('utf-8') if tls_key_file else None,
            tls_ca_file=tls_ca_file.encode('utf-8') if tls_ca_file else None,
            tls_verify_peer=tls_verify_peer,
            h2_push_enable=h2_push_enable,
            h2_max_push_per_request=h2_max_push_per_request,
            enable_simd=enable_simd,
            enable_arena_alignment=enable_arena_alignment,
            enable_openapi=enable_openapi
        )
        self._lib.csilk_app_set_server_config(self._app, config)

    @property
    def mq(self):
        if not hasattr(self, "_mq_obj"):
            server = self._lib.csilk_app_server(self._app)
            if server:
                mq_ptr = self._lib.csilk_server_get_mq(server)
                if mq_ptr:
                    self._mq_obj = MQ(mq_ptr)
                else:
                    return None
            else:
                return None
        return self._mq_obj

    def admin_serve(self, path):
        self._lib.csilk_admin_serve(self._app, path.encode('utf-8'))

    def admin_serve_secure(self, path, auth_middleware):
        import weakref
        app_ref = weakref.proxy(self)
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                auth_middleware(ctx)
            except Exception as e:
                if not app_ref._handle_exception(ctx, e):
                    import traceback
                    traceback.print_exc()
                    ctx.string(500, f"Internal Server Error: {str(e)}")

        self._handlers.append(wrapper)
        self._lib.csilk_admin_serve_secure(self._app, path.encode('utf-8'), wrapper)

    # Run / Stop
    def run(self, port):
        return self._lib.csilk_app_run(self._app, port)

    def stop(self):
        server = self._lib.csilk_app_server(self._app)
        if server:
            self._lib.csilk_server_stop(server)

    def on_server_start(self, callback):
        """Register a callback to run just before the event loop starts."""
        self._register_hook(0, callback, server_level=True)

    def on_server_stop(self, callback):
        """Register a callback to run when the server is shutting down."""
        self._register_hook(1, callback, server_level=True)

    def on_conn_open(self, callback):
        """Register a callback to run when a new TCP connection is opened."""
        self._register_hook(2, callback, server_level=False)

    def on_conn_close(self, callback):
        """Register a callback to run when a TCP connection is closed."""
        self._register_hook(3, callback, server_level=False)

    def on_request_begin(self, callback):
        """Register a callback to run when the HTTP request has been parsed."""
        self._register_hook(4, callback, server_level=False)

    def on_request_end(self, callback):
        """Register a callback to run after the response has been sent."""
        self._register_hook(5, callback, server_level=False)

    def _register_hook(self, hook_type, callback, server_level=False):
        server = self._lib.csilk_app_server(self._app)
        if not server:
            raise RuntimeError("Underlying server instance is not initialized")
            
        if server_level:
            @ctypes.CFUNCTYPE(None, ctypes.c_void_p)
            def wrapper(server_ptr):
                try:
                    callback(self)
                except Exception:
                    import traceback
                    traceback.print_exc()
        else:
            @ctypes.CFUNCTYPE(None, CsilkCtxPtr)
            def wrapper(ctx_ptr):
                ctx = Context(ctx_ptr)
                try:
                    callback(ctx)
                except Exception:
                    import traceback
                    traceback.print_exc()

        self._hooks.append(wrapper)
        self._lib.csilk_server_add_hook(server, hook_type, wrapper)

    def set_not_found_handler(self, handler):
        """Set a custom 404 handler for the server."""
        server = self._lib.csilk_app_server(self._app)
        if not server:
            raise RuntimeError("Underlying server instance is not initialized")
        
        from csilk.lib import CsilkHandler
        import weakref
        app_ref = weakref.proxy(self)
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                if not app_ref._handle_exception(ctx, e):
                    import traceback
                    traceback.print_exc()
                    ctx.string(500, f"Internal Server Error: {str(e)}")
                 
        self._handlers.append(wrapper)
        self._lib.csilk_server_set_not_found_handler(server, wrapper)

    def set_spa_fallback(self, doc_root):
        """Set Single Page Application (SPA) fallback directory."""
        server = self._lib.csilk_app_server(self._app)
        if not server:
            raise RuntimeError("Underlying server instance is not initialized")
        self._lib.csilk_server_set_spa_fallback(server, doc_root.encode('utf-8'))

    def set_max_connections(self, max_conn):
        """Set the maximum number of concurrent client connections."""
        server = self._lib.csilk_app_server(self._app)
        if not server:
            raise RuntimeError("Underlying server instance is not initialized")
        if self._lib.csilk_server_set_max_connections(server, max_conn) != 0:
            raise RuntimeError("Failed to set max connections")

    @property
    def server_stats(self):
        """Get active and pooled connections counts from the server."""
        server = self._lib.csilk_app_server(self._app)
        if not server:
            raise RuntimeError("Underlying server instance is not initialized")
        active_conn = ctypes.c_int(0)
        pooled_conn = ctypes.c_int(0)
        self._lib.csilk_server_get_stats(server, ctypes.byref(active_conn), ctypes.byref(pooled_conn))
        return {
            "active_connections": active_conn.value,
            "pooled_connections": pooled_conn.value
        }

    def __del__(self):
        self.free()

    def group(self, prefix):
        """Create a new top-level route group."""
        router = self._lib.csilk_app_router(self._app)
        if not router:
            raise RuntimeError("Failed to retrieve underlying router")
        g_ptr = self._lib.csilk_group_new(router, prefix.encode('utf-8'))
        if not g_ptr:
            raise RuntimeError(f"Failed to create route group with prefix '{prefix}'")
        g = Group(self, prefix, g_ptr)
        self._groups.append(g)
        return g

# Data validation decorator
def validate(body=None, query=None):
    """
    Decorator to validate request body and/or query parameters using Pydantic models.
    The validated models will be attached to ctx.validated_body and ctx.validated_query.
    """
    def decorator(handler):
        def wrapper(ctx):
            if body:
                try:
                    import json
                    data = json.loads(ctx.body.decode('utf-8'))
                    ctx.validated_body = body(**data)
                except Exception as e:
                    ctx.json_error(422, f"Validation Error (Body): {str(e)}")
                    ctx.abort()
                    return
            if query:
                try:
                    q_dict = ctx.queries
                    ctx.validated_query = query(**q_dict)
                except Exception as e:
                    ctx.json_error(422, f"Validation Error (Query): {str(e)}")
                    ctx.abort()
                    return
            return handler(ctx)
        return wrapper
    return decorator

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

def jwt_middleware(secret):
    c_secret = secret.encode('utf-8')
    def middleware(ctx):
        get_bindings().csilk_jwt_middleware(ctx._ctx, c_secret)
    return middleware


class MqContext:
    def __init__(self, ctx_ptr):
        self._ctx = ctx_ptr
        self._lib = get_bindings()

    @property
    def topic(self):
        res = self._lib.csilk_mq_get_topic(self._ctx)
        return res.decode('utf-8') if res else ""

    @property
    def payload(self):
        out_len = ctypes.c_size_t(0)
        res = self._lib.csilk_mq_get_payload(self._ctx, ctypes.byref(out_len))
        if res and out_len.value > 0:
            return ctypes.string_at(res, out_len.value)
        return b""

    def next(self):
        self._lib.csilk_mq_next(self._ctx)

    def abort(self):
        self._lib.csilk_mq_abort(self._ctx)

class MQ:
    def __init__(self, mq_ptr):
        self._mq = mq_ptr
        self._lib = get_bindings()
        self._handlers = []

    def subscribe(self, topic, handler=None):
        if handler is None:
            def decorator(h):
                self.subscribe(topic, h)
                return h
            return decorator

        from csilk.lib import CsilkMqHandler
        @CsilkMqHandler
        def wrapper(ctx_ptr):
            ctx = MqContext(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
        self._handlers.append(wrapper)
        self._lib.csilk_mq_subscribe(self._mq, topic.encode('utf-8'), wrapper)

    def use(self, topic, handler=None):
        if handler is None:
            def decorator(h):
                self.use(topic, h)
                return h
            return decorator

        from csilk.lib import CsilkMqHandler
        @CsilkMqHandler
        def wrapper(ctx_ptr):
            ctx = MqContext(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
        self._handlers.append(wrapper)
        c_topic = topic.encode('utf-8') if topic else None
        self._lib.csilk_mq_use(self._mq, c_topic, wrapper)

    def publish(self, topic, payload):
        if isinstance(payload, str):
            c_payload = payload.encode('utf-8')
        elif isinstance(payload, bytes):
            c_payload = payload
        else:
            c_payload = str(payload).encode('utf-8')
        return self._lib.csilk_mq_publish(self._mq, topic.encode('utf-8'), c_payload, len(c_payload))

    def set_persistence(self, wal_path):
        return self._lib.csilk_mq_set_persistence(self._mq, wal_path.encode('utf-8'))

    @property
    def stats(self):
        from csilk.lib import CsilkMqStats
        stats = CsilkMqStats()
        self._lib.csilk_mq_get_stats(self._mq, ctypes.byref(stats))
        return {
            "published_total": stats.published_total,
            "delivered_total": stats.delivered_total,
            "failed_total": stats.failed_total,
            "queue_depth": stats.queue_depth,
            "topic_count": stats.topic_count
        }


class Group:
    def __init__(self, app, prefix, group_ptr, parent=None):
        self._app = app
        self._prefix = prefix
        self._group = group_ptr
        self._parent = parent
        self._handlers = [] # To prevent GC of wrapper callbacks
        self._lib = app._lib

    def free(self):
        if hasattr(self, "_subgroups"):
            for sg in self._subgroups:
                sg.free()
            self._subgroups.clear()
        if self._group:
            self._lib.csilk_group_free(self._group)
            self._group = None
        self._handlers.clear()

    def __del__(self):
        self.free()

    def use(self, middleware):
        import weakref
        app_ref = weakref.proxy(self._app)
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                middleware(ctx)
            except Exception as e:
                if not app_ref._handle_exception(ctx, e):
                    import traceback
                    traceback.print_exc()
                    ctx.string(500, f"Internal Server Error: {str(e)}")
        self._handlers.append(wrapper)
        self._lib.csilk_group_use(self._group, wrapper)

    def route(self, method, path, handler, input_type=None, output_type=None, summary=None, description=None, perm_required=None, perm_resource=None):
        import asyncio
        import weakref
        is_coro = asyncio.iscoroutinefunction(handler)
        app_ref = weakref.proxy(self._app)

        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            ctx._register_ws = lambda: app_ref._websocket_contexts.update({ctypes.addressof(ctx_ptr.contents): ctx})
            
            if is_coro:
                ctx.dispatch_async(handler, app_ref._loop)
            else:
                try:
                    handler(ctx)
                except Exception as e:
                    if not app_ref._handle_exception(ctx, e):
                        import traceback
                        traceback.print_exc()
                        ctx.string(500, f"Internal Server Error: {str(e)}")
                finally:
                    self._lib.csilk_ctx_cleanup_jwt_payload(ctx_ptr)

                if ctx.is_websocket:
                    self._app._websocket_contexts[ctypes.addressof(ctx_ptr.contents)] = ctx

        # Keep callback reference alive in this group (and transitively in app)
        self._handlers.append(wrapper)
        self._app._handlers.append(wrapper)

        self._lib.csilk_group_add_route_extended_perm(
            self._group,
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

    def ws(self, path, ws_class=None, **kwargs):
        """Register a class-based WebSocket handler."""
        def decorator(cls):
            def handler(ctx):
                instance = cls()
                ctx.ws_handshake()
                if hasattr(instance, "on_connect"):
                    instance.on_connect(ctx)
                if hasattr(instance, "on_message"):
                    def on_msg(c, msg, op):
                        instance.on_message(c, msg, op)
                    ctx.set_on_ws_message(on_msg)
            self.route("GET", path, handler, **kwargs)
            return cls
        if ws_class is None:
            return decorator
        return decorator(ws_class)

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

    def group(self, prefix):
        """Create a nested route group."""
        g_ptr = self._lib.csilk_group_group(self._group, prefix.encode('utf-8'))
        if not g_ptr:
            raise RuntimeError(f"Failed to create sub-group with prefix '{prefix}'")
        g = Group(self._app, prefix, g_ptr, parent=self)
        if not hasattr(self, "_subgroups"):
            self._subgroups = []
        self._subgroups.append(g)
        return g
