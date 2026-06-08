import ctypes
from csilk.lib import get_bindings, CsilkHandler
from csilk.context import Context

class App:
    def __init__(self, config_path=None):
        self._lib = get_bindings()
        c_config = config_path.encode('utf-8') if config_path else None
        self._app = self._lib.csilk_app_new(c_config)
        if not self._app:
            raise RuntimeError("Failed to create csilk application instance")
        self._handlers = [] # Reference storage to prevent garbage collection of callbacks

    def free(self):
        if self._app:
            self._lib.csilk_app_free(self._app)
            self._app = None
        self._handlers.clear()

    def route(self, method, path, handler):
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
        self._lib.csilk_app_add_route(
            self._app,
            method.encode('utf-8'),
            path.encode('utf-8'),
            wrapper
        )

    def get(self, path, handler):
        self.route("GET", path, handler)

    def post(self, path, handler):
        self.route("POST", path, handler)

    def run(self, port):
        return self._lib.csilk_app_run(self._app, port)

    def stop(self):
        server = self._lib.csilk_app_server(self._app)
        if server:
            self._lib.csilk_server_stop(server)

    def __del__(self):
        self.free()
