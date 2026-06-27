"""Request context, session management, and response helpers.

Provides the core ``Context`` object that every route handler receives,
along with session storage and a case-insensitive dict for headers.
"""

import ctypes
import json
import time
from csilk.lib import get_bindings

_python_sessions = {}


class Session:
    """Dict-like session storage backed by an in-process dict.

    Sessions are identified by the session ID managed by the C layer.
    Data expires after 24 hours of inactivity.

    Usage::

        ctx.session_start()
        ctx.session["user_id"] = 42
        print(ctx.session["user_id"])
    """

    def __init__(self, ctx):
        self._ctx = ctx
        self._lib = ctx._lib

    @property
    def id(self) -> str:
        """The session ID string, or ``None`` if no session is active."""
        res = self._lib.csilk_session_get_id(self._ctx._ctx)
        return res.decode('utf-8') if res else None

    def _prune(self):
        now = time.time()
        for sid, s_dict in list(_python_sessions.items()):
            if s_dict.get("_expires_at", 0) < now:
                _python_sessions.pop(sid, None)

    def __getitem__(self, key):
        self._prune()
        sid = self.id
        if not sid:
            return None
        s_dict = _python_sessions.get(sid)
        if s_dict:
            s_dict["_expires_at"] = time.time() + 86400
            return s_dict["_data"].get(key)
        return None

    def __setitem__(self, key, value):
        self._prune()
        sid = self.id
        if not sid:
            raise RuntimeError("Session not started. Call session_start() first.")
        if sid not in _python_sessions:
            _python_sessions[sid] = {"_data": {}, "_expires_at": 0}
        _python_sessions[sid]["_data"][key] = value
        _python_sessions[sid]["_expires_at"] = time.time() + 86400

    def __delitem__(self, key):
        self._prune()
        sid = self.id
        if sid and sid in _python_sessions:
            _python_sessions[sid]["_data"].pop(key, None)

    def __contains__(self, key):
        self._prune()
        sid = self.id
        if not sid:
            return False
        s_dict = _python_sessions.get(sid)
        return key in s_dict["_data"] if s_dict else False

    def get(self, key, default=None):
        """Return a session value, or *default* if missing."""
        try:
            val = self[key]
            return val if val is not None else default
        except KeyError:
            return default

    def pop(self, key, default=None):
        """Remove and return a session value, or *default*."""
        self._prune()
        sid = self.id
        if not sid:
            return default
        s_dict = _python_sessions.get(sid)
        if s_dict:
            return s_dict["_data"].pop(key, default)
        return default

    def clear(self):
        """Remove all values from the current session."""
        self._prune()
        sid = self.id
        if sid and sid in _python_sessions:
            _python_sessions[sid]["_data"].clear()

class CaseInsensitiveDict(dict):
    """A dict whose string-key lookups are case-insensitive.

    The original key casing is preserved for iteration; only lookups
    (``__getitem__``, ``__contains__``, ``get``) are folded to lowercase.
    """

    def __init__(self, data=None, **kwargs):
        super().__init__()
        self._store = {}
        if data:
            self.update(data)
        if kwargs:
            self.update(kwargs)

    def __setitem__(self, key, value):
        lk = key.lower()
        if lk in self._store:
            old_key, _ = self._store[lk]
            if old_key != key:
                super().__delitem__(old_key)
        self._store[lk] = (key, value)
        super().__setitem__(key, value)

    def __getitem__(self, key):
        return self._store[key.lower()][1]

    def __delitem__(self, key):
        lk = key.lower()
        old_key, _ = self._store.pop(lk)
        super().__delitem__(old_key)

    def __contains__(self, key):
        return key.lower() in self._store

    def get(self, key, default=None):
        try:
            return self[key]
        except KeyError:
            return default

    def update(self, E=None, **F):
        if E is not None:
            if hasattr(E, 'keys'):
                for k in E:
                    self[k] = E[k]
            else:
                for k, v in E:
                    self[k] = v
        for k in F:
            self[k] = F[k]

    def pop(self, key, default=None):
        try:
            val = self[key]
            self.__delitem__(key)
            return val
        except KeyError:
            return default

_dispatcher_map = {}

@ctypes.CFUNCTYPE(None, ctypes.c_void_p)
def _python_dispatcher(arg):
    func_id = arg
    func = _dispatcher_map.pop(func_id, None)
    if func:
        try:
            func()
        except Exception as e:
            import traceback
            traceback.print_exc()

class Context:
    """Request context — the primary object passed to every route handler.

    Provides access to the request (method, path, body, headers, query
    params, cookies, etc.) and methods for writing the response (string,
    JSON, file, redirect, SSE, WebSocket frames).

    Usage::

        def my_handler(ctx: Context):
            name = ctx.query("name") or "world"
            ctx.string(200, f"Hello {name}!")
    """

    def __init__(self, ctx_ptr):
        """Wrap a C-level ``csilk_ctx_t`` pointer.

        Args:
            ctx_ptr: The opaque ``CsilkCtxPtr`` from the C layer.
        """
        self._ctx = ctx_ptr
        self._lib = get_bindings()

    # ── Request properties ──────────────────────────────────────────

    @property
    def method(self) -> str:
        """HTTP method of the current request (GET, POST, …)."""
        res = self._lib.csilk_get_method(self._ctx)
        return res.decode('utf-8') if res else ""

    @property
    def path(self) -> str:
        """Raw URL path of the current request."""
        res = self._lib.csilk_get_path(self._ctx)
        return res.decode('utf-8') if res else ""

    @property
    def body(self) -> bytes:
        """Raw request body as ``bytes``."""
        length = ctypes.c_size_t()
        ptr = self._lib.csilk_get_body(self._ctx, ctypes.byref(length))
        if ptr and length.value > 0:
            return ctypes.string_at(ptr, length.value)
        return b""

    @property
    def body_view(self):
        """Zero-copy ``memoryview`` of the request body."""
        length = ctypes.c_size_t()
        ptr = self._lib.csilk_get_body(self._ctx, ctypes.byref(length))
        if ptr and length.value > 0:
            ArrayType = ctypes.c_char * length.value
            array = ArrayType.from_address(ptr)
            return memoryview(array)
        return None

    @property
    def body_len(self) -> int:
        """Length of the request body in bytes."""
        return self._lib.csilk_get_body_len(self._ctx)

    @property
    def is_websocket(self) -> bool:
        """``True`` if the request has been upgraded to a WebSocket."""
        return bool(self._lib.csilk_is_websocket(self._ctx))

    @property
    def is_sse(self) -> bool:
        """``True`` if the connection is in SSE mode."""
        return bool(self._lib.csilk_is_sse(self._ctx))

    @property
    def is_async(self) -> bool:
        """``True`` if the handler is running in async (deferred) mode."""
        return bool(self._lib.csilk_is_async(self._ctx))

    @is_async.setter
    def is_async(self, val: bool):
        self._lib.csilk_ctx_set_async(self._ctx, 1 if val else 0)

    @property
    def is_aborted(self) -> bool:
        """``True`` if the middleware chain has been aborted."""
        return bool(self._lib.csilk_is_aborted(self._ctx))

    # ── Request metadata ────────────────────────────────────────────

    @property
    def request_id(self) -> str:
        """The ``X-Request-Id`` header value, or empty string."""
        res = self._lib.csilk_get_request_id(self._ctx)
        return res.decode('utf-8') if res else ""

    @request_id.setter
    def request_id(self, val: str):
        self._lib.csilk_set_request_id(self._ctx, val.encode('utf-8') if val else None)

    @property
    def status_code(self) -> int:
        """The HTTP response status code."""
        return self._lib.csilk_get_status(self._ctx)

    @status_code.setter
    def status_code(self, val: int):
        self._lib.csilk_set_status(self._ctx, int(val))

    def query(self, key: str) -> str:
        """Return a single query-string parameter value, or ``None``."""
        res = self._lib.csilk_get_query(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def param(self, key: str) -> str:
        """Return a single route path parameter value, or ``None``."""
        res = self._lib.csilk_get_param(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    @property
    def params(self) -> dict:
        """All route path parameters as a ``{name: value}`` dict."""
        count = self._lib.csilk_get_params_count(self._ctx)
        p_dict = {}
        for i in range(count):
            key = self._lib.csilk_get_param_key(self._ctx, i)
            val = self._lib.csilk_get_param_value(self._ctx, i)
            if key and val:
                p_dict[key.decode('utf-8')] = val.decode('utf-8')
        return p_dict

    def get_header(self, key: str) -> str:
        """Return a request header value, or ``None``."""
        res = self._lib.csilk_get_header(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def get_response_header(self, key: str) -> str:
        """Return a response header value, or ``None``."""
        res = self._lib.csilk_get_response_header(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def set_request_header(self, key: str, value: str):
        """Set or override a request header before it reaches downstream handlers."""
        self._lib.csilk_set_request_header(
            self._ctx,
            key.encode('utf-8'),
            value.encode('utf-8') if value else None,
        )

    # ── Response helpers ────────────────────────────────────────────

    def status(self, code: int):
        """Set the HTTP response status code (without writing body)."""
        self._lib.csilk_status(self._ctx, code)

    def write(self, data):
        """Write raw bytes to the response body.

        Accepts ``str``, ``bytes``, ``bytearray``, and ``memoryview``.
        """
        if isinstance(data, str):
            c_data = data.encode('utf-8')
            self._lib.csilk_response_write(self._ctx, c_data, len(c_data))
        elif isinstance(data, memoryview):
            arr = (ctypes.c_char * data.nbytes).from_buffer(data)
            self._lib.csilk_response_write(self._ctx, arr, data.nbytes)
        elif isinstance(data, bytearray):
            arr = (ctypes.c_char * len(data)).from_buffer(data)
            self._lib.csilk_response_write(self._ctx, arr, len(data))
        else:
            c_data = bytes(data)
            self._lib.csilk_response_write(self._ctx, c_data, len(c_data))

    def string(self, code: int, text: str):
        """Send a plain-text response with the given status code."""
        self._lib.csilk_string(self._ctx, code, text.encode('utf-8'))

    def json(self, code: int, data):
        """Send a JSON response with the given status code.

        *data* is serialised via ``json.dumps`` and the
        ``Content-Type`` is set to ``application/json``.
        """
        json_str = json.dumps(data)
        self.set_header("Content-Type", "application/json")
        self._lib.csilk_string(self._ctx, code, json_str.encode('utf-8'))

    def json_error(self, code: int, message: str):
        """Send a JSON error response with the given status code and message."""
        self._lib.csilk_json_error(self._ctx, code, message.encode('utf-8'))

    def set_header(self, key: str, value: str):
        """Set a response header (overwriting any existing value)."""
        self._lib.csilk_set_header(
            self._ctx,
            key.encode('utf-8'),
            value.encode('utf-8') if value else None,
        )

    def add_header(self, key: str, value: str):
        """Add a response header without removing existing values for the same key."""
        self._lib.csilk_add_header(
            self._ctx,
            key.encode('utf-8'),
            value.encode('utf-8') if value else None,
        )

    def file(self, path: str):
        """Send a file as the response (uses ``sendfile`` internally)."""
        self._lib.csilk_file(self._ctx, path.encode('utf-8'))

    def redirect(self, code: int, location: str):
        """Send an HTTP redirect with the given status code and ``Location``."""
        self._lib.csilk_redirect(self._ctx, code, location.encode('utf-8'))

    def redirect_simple(self, location: str):
        """Send a ``302 Found`` redirect to *location*."""
        self._lib.csilk_redirect_simple(self._ctx, location.encode('utf-8'))

    # ── Middleware flow control ──────────────────────────────────────

    def next(self):
        """Pass control to the next handler in the middleware chain."""
        self._lib.csilk_next(self._ctx)

    def abort(self):
        """Abort the middleware chain immediately (no further handlers run)."""
        self._lib.csilk_abort(self._ctx)

    # ── WebSockets ──────────────────────────────────────────────────

    def ws_handshake(self):
        """Perform the WebSocket upgrade handshake."""
        self._lib.csilk_ws_handshake(self._ctx)
        if hasattr(self, "_register_ws"):
            self._register_ws()

    def set_on_ws_message(self, callback):
        """Register a callback for incoming WebSocket messages.

        The callback receives ``(ctx, payload: bytes, opcode: int)``.
        """
        from csilk.lib import CsilkWsMessageCallback

        @CsilkWsMessageCallback
        def wrapper(ctx_ptr, payload_ptr, length, opcode):
            payload = ctypes.string_at(payload_ptr, length)
            callback(self, payload, opcode)

        self._ws_msg_callback = wrapper
        self._lib.csilk_set_on_ws_message(self._ctx, wrapper)

    def ws_send(self, payload, opcode: int = 0x1):
        """Send a WebSocket frame.

        Args:
            payload: String or bytes to send.
            opcode: WebSocket opcode (``0x1`` = text, ``0x2`` = binary).
        """
        if isinstance(payload, str):
            c_payload = payload.encode('utf-8')
        else:
            c_payload = bytes(payload)
        self._lib.csilk_ws_send(self._ctx, c_payload, len(c_payload), opcode)

    def ws_close(self, status_code: int = 1000, reason: str = None):
        """Close the WebSocket connection."""
        c_reason = reason.encode('utf-8') if reason else None
        self._lib.csilk_ws_close(self._ctx, status_code, c_reason)

    def ws_join_room(self, room_name: str):
        """Join the WebSocket connection to a broadcast room."""
        self._lib.csilk_ws_join_room(self._ctx, room_name.encode('utf-8'))

    def ws_leave_room(self, room_name: str):
        """Leave a broadcast room."""
        self._lib.csilk_ws_leave_room(self._ctx, room_name.encode('utf-8'))

    def ws_broadcast_room(self, room_name: str, message: str):
        """Broadcast a message to all connections in a room."""
        self._lib.csilk_ws_broadcast_room(
            self._ctx,
            room_name.encode('utf-8'),
            message.encode('utf-8'),
        )

    # ── Server-Sent Events (SSE) ────────────────────────────────────

    def sse_init(self):
        """Initialise an SSE connection (sets headers + flushes)."""
        self._lib.csilk_sse_init(self._ctx)

    def sse_send(self, event: str, data: str):
        """Send an SSE event."""
        c_event = event.encode('utf-8') if event else None
        c_data = data.encode('utf-8') if data else None
        self._lib.csilk_sse_send(self._ctx, c_event, c_data)

    def sse_close(self):
        """Close the SSE connection."""
        self._lib.csilk_sse_close(self._ctx)

    # ── Streaming and HTTP/2 Push ────────────────────────────────────

    def response_write(self, data):
        """Write data and end the response (convenience wrapper)."""
        self.write(data)

        if not getattr(self, '_response_ended', False) and not self._lib.csilk_is_async(self._ctx):
            self.response_end()

    def dispatch_async(self, coro, loop):
        """Execute an asyncio coroutine on the given event loop.

        The result is dispatched back to the C event loop thread for
        sending the response.

        Args:
            coro: An async function that receives ``self`` (the Context).
            loop: The ``asyncio.AbstractEventLoop`` to run on.
        """
        import asyncio
        import traceback

        self.is_async = True

        async def _runner():
            try:
                response_data = await coro(self)

                def _send():
                    if isinstance(response_data, dict):
                        self.json(200, response_data)
                    elif response_data is not None:
                        self.string(200, str(response_data))
                    self.response_end()

                self.dispatch(_send)
            except Exception as e:
                import traceback
                traceback.print_exc()
                error_msg = str(e)

                def _send_error():
                    self.string(500, f"Internal Server Error: {error_msg}")
                    self.response_end()

                self.dispatch(_send_error)

        asyncio.run_coroutine_threadsafe(_runner(), loop)

    def dispatch(self, func):
        """Schedule *func* to run on the C event-loop thread.

        This is the safe way to send a response from a background /
        async thread.
        """
        func_id = id(func)
        _dispatcher_map[func_id] = func
        self._lib.csilk_dispatch(self._ctx, _python_dispatcher, func_id)

    def response_end(self):
        """Finalise and flush the HTTP response.

        No further ``write`` / ``string`` / ``json`` calls are allowed
        after this.
        """
        if not getattr(self, '_response_ended', False):
            self._response_ended = True
            self._lib.csilk_response_end(self._ctx)

    def push_promise(self, method: str, path: str) -> int:
        """Issue an HTTP/2 server push promise.

        Returns:
            ``0`` on success, nonzero on failure.
        """
        return self._lib.csilk_push_promise(
            self._ctx,
            method.encode('utf-8'),
            path.encode('utf-8'),
        )

    # ── Cookies ─────────────────────────────────────────────────────

    def get_cookie(self, name: str) -> str:
        """Return a cookie value, or ``None``."""
        res = self._lib.csilk_get_cookie(self._ctx, name.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def set_cookie(self, name: str, value: str, max_age: int = 0,
                   path: str = None, domain: str = None,
                   secure: bool = False, http_only: bool = False):
        """Set a response cookie."""
        self._lib.csilk_set_cookie(
            self._ctx,
            name.encode('utf-8'),
            value.encode('utf-8'),
            max_age,
            path.encode('utf-8') if path else None,
            domain.encode('utf-8') if domain else None,
            1 if secure else 0,
            1 if http_only else 0,
        )

    # ── Form parsing ────────────────────────────────────────────────

    def parse_form_urlencoded(self):
        """Parse the request body as ``application/x-www-form-urlencoded``."""
        self._lib.csilk_parse_form_urlencoded(self._ctx)

    # ── Hash map properties (iterated from C) ────────────────────────

    @property
    def headers(self) -> CaseInsensitiveDict:
        """All request headers as a case-insensitive dict."""
        from csilk.lib import CsilkHeaderCb

        h_dict = CaseInsensitiveDict()

        @CsilkHeaderCb
        def cb(key, value, arg):
            h_dict[key.decode('latin-1')] = value.decode('latin-1')
            return 1

        self._lib.csilk_for_each_header(self._ctx, cb, None)
        return h_dict

    @property
    def queries(self) -> dict:
        """All query-string parameters as a dict."""
        from csilk.lib import CsilkHeaderCb

        q_dict = {}

        @CsilkHeaderCb
        def cb(key, value, arg):
            q_dict[key.decode('latin-1')] = value.decode('latin-1')
            return 1

        self._lib.csilk_for_each_query(self._ctx, cb, None)
        return q_dict

    @property
    def form_fields(self) -> dict:
        """All parsed form fields as a dict."""
        from csilk.lib import CsilkHeaderCb

        f_dict = {}

        @CsilkHeaderCb
        def cb(key, value, arg):
            f_dict[key.decode('latin-1')] = value.decode('latin-1')
            return 1

        self._lib.csilk_for_each_form_field(self._ctx, cb, None)
        return f_dict

    # ── Handler metadata ────────────────────────────────────────────

    @property
    def handler_path(self) -> str:
        """The matched route pattern for the current handler."""
        res = self._lib.csilk_ctx_get_handler_path(self._ctx)
        return res.decode('utf-8') if res else None

    @property
    def handler_perm_required(self) -> str:
        """The permission string required for the current handler."""
        res = self._lib.csilk_ctx_get_handler_perm_required(self._ctx)
        return res.decode('utf-8') if res else None

    @property
    def handler_perm_resource(self) -> str:
        """The permission resource type for the current handler."""
        res = self._lib.csilk_ctx_get_handler_perm_resource(self._ctx)
        return res.decode('utf-8') if res else None

    # ── KV storage (TTL-backed) ─────────────────────────────────────

    def set_string(self, key: str, value: str, ttl_sec: int = 0) -> int:
        """Store a string in the request-scoped key-value store with optional TTL.

        Returns:
            ``0`` on success, nonzero on failure.
        """
        return self._lib.csilk_set_string(self._ctx, key.encode('utf-8'),
                                           value.encode('utf-8'), ttl_sec)

    def get_string(self, key: str) -> str:
        """Retrieve a string from the request-scoped key-value store."""
        res = self._lib.csilk_get_string(self._ctx, key.encode('utf-8'))
        if res:
            val = ctypes.string_at(res).decode('utf-8')
            self._lib.csilk_free(res)
            return val
        return None

    def incr(self, key: str, ttl_sec: int = 0) -> int:
        """Atomically increment a numeric value, returning the new value."""
        return self._lib.csilk_incr(self._ctx, key.encode('utf-8'), ttl_sec)

    # ── Response body mutation ──────────────────────────────────────

    @property
    def response_body(self) -> bytes:
        """The current response body as ``bytes``."""
        out_len = ctypes.c_size_t(0)
        res = self._lib.csilk_get_response_body(self._ctx, ctypes.byref(out_len))
        if res and out_len.value > 0:
            return ctypes.string_at(res, out_len.value)
        return b""

    @response_body.setter
    def response_body(self, body):
        """Replace the response body in-place."""
        if isinstance(body, str):
            self._response_body_ref = body.encode('utf-8')
            c_ptr, size = self._response_body_ref, len(self._response_body_ref)
        elif isinstance(body, memoryview):
            self._response_body_ref = body
            c_ptr = (ctypes.c_char * body.nbytes).from_buffer(body)
            size = body.nbytes
        elif isinstance(body, bytearray):
            self._response_body_ref = body
            c_ptr = (ctypes.c_char * len(body)).from_buffer(body)
            size = len(body)
        else:
            self._response_body_ref = bytes(body)
            c_ptr, size = self._response_body_ref, len(self._response_body_ref)

        self._lib.csilk_set_response_body(self._ctx, c_ptr, size, 0)

    # ── Session management ──────────────────────────────────────────

    @property
    def session(self) -> Session:
        """The ``Session`` object for this request (lazily created)."""
        if not hasattr(self, "_session_obj"):
            self._session_obj = Session(self)
        return self._session_obj

    def session_start(self):
        """Initialise a new session for this request."""
        self._lib.csilk_session_start(self._ctx)

    def session_destroy(self):
        """Destroy the current session."""
        sid = self.session_id
        if sid:
            _python_sessions.pop(sid, None)
        self._lib.csilk_session_destroy(self._ctx)

    @property
    def session_id(self) -> str:
        """The current session ID, or ``None``."""
        res = self._lib.csilk_session_get_id(self._ctx)
        return res.decode('utf-8') if res else None

    # ── JWT helpers ─────────────────────────────────────────────────

    @property
    def jwt_payload(self) -> dict:
        """The decoded JWT payload (from the auth middleware)."""
        if not hasattr(self, "_jwt_payload"):
            res = self._lib.csilk_ctx_get_jwt_payload_json(self._ctx)
            if res:
                val = ctypes.string_at(res).decode('utf-8')
                self._lib.csilk_free(res)
                self._jwt_payload = json.loads(val)
            else:
                self._jwt_payload = None
        return self._jwt_payload

    def jwt_generate(self, payload: dict, secret: str) -> str:
        """Generate a signed JWT token.

        Args:
            payload: The JWT payload claims.
            secret: HMAC secret key.

        Returns:
            The encoded JWT string, or ``None`` on failure.
        """
        payload_json = json.dumps(payload).encode('utf-8')
        c_secret = secret.encode('utf-8')
        res = self._lib.csilk_jwt_generate_json(self._ctx, payload_json, c_secret)
        if res:
            token = ctypes.string_at(res).decode('utf-8')
            self._lib.csilk_free(res)
            return token
        return None

    # ── Request-local storage (opaque pointers) ─────────────────────

    def set(self, key: str, value):
        """Store an arbitrary Python value on the context (request-scoped)."""
        if not hasattr(self, "_storage"):
            self._storage = {}
        key_bytes = key.encode('utf-8')
        if isinstance(value, str):
            value_bytes = value.encode('utf-8')
            self._storage[key] = value_bytes
            c_ptr = ctypes.cast(ctypes.c_char_p(value_bytes), ctypes.c_void_p)
            self._lib.csilk_set(self._ctx, key_bytes, c_ptr)
        elif isinstance(value, bytes):
            self._storage[key] = value
            c_ptr = ctypes.cast(ctypes.c_char_p(value), ctypes.c_void_p)
            self._lib.csilk_set(self._ctx, key_bytes, c_ptr)
        else:
            self._storage[key] = value
            self._lib.csilk_set(self._ctx, key_bytes, ctypes.c_void_p(id(value)))

    def get(self, key: str):
        """Retrieve a value previously stored via ``set()``."""
        key_bytes = key.encode('utf-8')
        ptr = self._lib.csilk_get(self._ctx, key_bytes)
        if not ptr:
            return None
        if hasattr(self, "_storage") and key in self._storage:
            val = self._storage[key]
            if isinstance(val, bytes):
                return val.decode('utf-8')
            return val
        return ctypes.string_at(ptr).decode('utf-8')

    # ── Test utilities ──────────────────────────────────────────────

    @classmethod
    def create_test_context(cls):
        """Create a ``Context`` backed by a test (mock) C context.

        Use this in unit tests to simulate a request context without a
        running server.
        """
        lib = get_bindings()
        ctx_ptr = lib.csilk_test_ctx_new()
        if not ctx_ptr:
            raise RuntimeError("Failed to create test context")
        ctx = cls(ctx_ptr)
        ctx._owned = True
        return ctx

    def free_test_context(self):
        """Free a test context created by ``create_test_context``."""
        if hasattr(self, "_owned") and self._owned and self._ctx:
            self._lib.csilk_test_ctx_free(self._ctx)
            self._ctx = None
            self._owned = False

    # ── Multipart parsing ───────────────────────────────────────────

    def multipart_parse(self, handler):
        """Parse a ``multipart/form-data`` request body.

        Each part is passed to *handler* as a dict with keys ``name``,
        ``filename``, ``content_type``, and ``data``.
        """
        from csilk.lib import CsilkMultipartHandler

        @CsilkMultipartHandler
        def wrapper(part_ptr):
            try:
                name = part_ptr.contents.name.decode('utf-8')
                filename = part_ptr.contents.filename.decode('utf-8') if part_ptr.contents.filename else ""
                content_type = part_ptr.contents.content_type.decode('utf-8') if part_ptr.contents.content_type else ""

                data_len = part_ptr.contents.data_len
                data_bytes = b""
                if data_len > 0 and part_ptr.contents.data:
                    data_bytes = ctypes.string_at(part_ptr.contents.data, data_len)

                part_dict = {
                    "name": name,
                    "filename": filename if filename else None,
                    "content_type": content_type if content_type else None,
                    "data": data_bytes,
                }
                handler(part_dict)
            except Exception as e:
                import traceback
                traceback.print_exc()

        self._lib.csilk_multipart_parse(self._ctx, wrapper)


