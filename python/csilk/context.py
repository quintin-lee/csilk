import ctypes
import json
import time
from csilk.lib import get_bindings

_python_sessions = {}

class Session:
    def __init__(self, ctx):
        self._ctx = ctx
        self._lib = ctx._lib

    @property
    def id(self):
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
        try:
            val = self[key]
            return val if val is not None else default
        except KeyError:
            return default

    def pop(self, key, default=None):
        self._prune()
        sid = self.id
        if not sid:
            return default
        s_dict = _python_sessions.get(sid)
        if s_dict:
            return s_dict["_data"].pop(key, default)
        return default

    def clear(self):
        self._prune()
        sid = self.id
        if sid and sid in _python_sessions:
            _python_sessions[sid]["_data"].clear()

class CaseInsensitiveDict(dict):
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
    def __init__(self, ctx_ptr):
        self._ctx = ctx_ptr
        self._lib = get_bindings()

    @property
    def method(self):
        res = self._lib.csilk_get_method(self._ctx)
        return res.decode('utf-8') if res else ""

    @property
    def path(self):
        res = self._lib.csilk_get_path(self._ctx)
        return res.decode('utf-8') if res else ""

    @property
    def body(self):
        length = ctypes.c_size_t()
        ptr = self._lib.csilk_get_body(self._ctx, ctypes.byref(length))
        if ptr and length.value > 0:
            return ctypes.string_at(ptr, length.value)
        return b""

    @property
    def body_view(self):
        """Zero-copy memoryview of the request body."""
        length = ctypes.c_size_t()
        ptr = self._lib.csilk_get_body(self._ctx, ctypes.byref(length))
        if ptr and length.value > 0:
            ArrayType = ctypes.c_char * length.value
            array = ArrayType.from_address(ptr)
            return memoryview(array)
        return None

    @property
    def body_len(self):
        return self._lib.csilk_get_body_len(self._ctx)

    @property
    def is_websocket(self):
        return bool(self._lib.csilk_is_websocket(self._ctx))

    @property
    def is_sse(self):
        return bool(self._lib.csilk_is_sse(self._ctx))

    @property
    def is_async(self):
        return bool(self._lib.csilk_is_async(self._ctx))

    @is_async.setter
    def is_async(self, val):
        self._lib.csilk_ctx_set_async(self._ctx, 1 if val else 0)

    @property
    def is_aborted(self):
        return bool(self._lib.csilk_is_aborted(self._ctx))

    @property
    def request_id(self):
        res = self._lib.csilk_get_request_id(self._ctx)
        return res.decode('utf-8') if res else ""

    @request_id.setter
    def request_id(self, val):
        self._lib.csilk_set_request_id(self._ctx, val.encode('utf-8') if val else None)

    @property
    def status_code(self):
        return self._lib.csilk_get_status(self._ctx)

    @status_code.setter
    def status_code(self, val):
        self._lib.csilk_set_status(self._ctx, int(val))

    def query(self, key):
        res = self._lib.csilk_get_query(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def param(self, key):
        res = self._lib.csilk_get_param(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    @property
    def params(self):
        """Returns a dict of all route path parameters."""
        count = self._lib.csilk_get_params_count(self._ctx)
        p_dict = {}
        for i in range(count):
            key = self._lib.csilk_get_param_key(self._ctx, i)
            val = self._lib.csilk_get_param_value(self._ctx, i)
            if key and val:
                p_dict[key.decode('utf-8')] = val.decode('utf-8')
        return p_dict

    def get_header(self, key):
        res = self._lib.csilk_get_header(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def get_response_header(self, key):
        res = self._lib.csilk_get_response_header(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def set_request_header(self, key, value):
        self._lib.csilk_set_request_header(
            self._ctx, 
            key.encode('utf-8'), 
            value.encode('utf-8') if value else None
        )

    def status(self, code):
        self._lib.csilk_status(self._ctx, code)

    def write(self, data):
        if isinstance(data, str):
            c_data = data.encode('utf-8')
            self._lib.csilk_response_write(self._ctx, c_data, len(c_data))
        elif isinstance(data, memoryview):
            # Zero-copy pointer extraction from memoryview
            arr = (ctypes.c_char * data.nbytes).from_buffer(data)
            self._lib.csilk_response_write(self._ctx, arr, data.nbytes)
        elif isinstance(data, bytearray):
            arr = (ctypes.c_char * len(data)).from_buffer(data)
            self._lib.csilk_response_write(self._ctx, arr, len(data))
        else:
            c_data = bytes(data)
            self._lib.csilk_response_write(self._ctx, c_data, len(c_data))

    def response_end(self):
        if not getattr(self, '_response_ended', False):
            self._response_ended = True
            self._lib.csilk_response_end(self._ctx)

    def string(self, code, text):
        self._lib.csilk_string(self._ctx, code, text.encode('utf-8'))

    def json(self, code, data):
        json_str = json.dumps(data)
        self.set_header("Content-Type", "application/json")
        self._lib.csilk_string(self._ctx, code, json_str.encode('utf-8'))

    def json_error(self, code, message):
        self._lib.csilk_json_error(self._ctx, code, message.encode('utf-8'))

    def set_header(self, key, value):
        self._lib.csilk_set_header(
            self._ctx, 
            key.encode('utf-8'), 
            value.encode('utf-8') if value else None
        )

    def add_header(self, key, value):
        self._lib.csilk_add_header(
            self._ctx,
            key.encode('utf-8'),
            value.encode('utf-8') if value else None
        )

    def file(self, path):
        self._lib.csilk_file(self._ctx, path.encode('utf-8'))

    def redirect(self, code, location):
        self._lib.csilk_redirect(self._ctx, code, location.encode('utf-8'))

    def redirect_simple(self, location):
        self._lib.csilk_redirect_simple(self._ctx, location.encode('utf-8'))

    # Middleware flow control
    def next(self):
        self._lib.csilk_next(self._ctx)

    def abort(self):
        self._lib.csilk_abort(self._ctx)

    # WebSockets
    def ws_handshake(self):
        self._lib.csilk_ws_handshake(self._ctx)
        if hasattr(self, "_register_ws"):
            self._register_ws()

    def set_on_ws_message(self, callback):
        from csilk.lib import CsilkWsMessageCallback
        @CsilkWsMessageCallback
        def wrapper(ctx_ptr, payload_ptr, length, opcode):
            payload = ctypes.string_at(payload_ptr, length)
            callback(self, payload, opcode)
        self._ws_msg_callback = wrapper
        self._lib.csilk_set_on_ws_message(self._ctx, wrapper)

    def ws_send(self, payload, opcode=0x1):
        if isinstance(payload, str):
            c_payload = payload.encode('utf-8')
        else:
            c_payload = bytes(payload)
        self._lib.csilk_ws_send(self._ctx, c_payload, len(c_payload), opcode)

    def ws_close(self, status_code=1000, reason=None):
        c_reason = reason.encode('utf-8') if reason else None
        self._lib.csilk_ws_close(self._ctx, status_code, c_reason)

    def ws_join_room(self, room_name):
        self._lib.csilk_ws_join_room(self._ctx, room_name.encode('utf-8'))

    def ws_leave_room(self, room_name):
        self._lib.csilk_ws_leave_room(self._ctx, room_name.encode('utf-8'))

    def ws_broadcast_room(self, room_name, message):
        self._lib.csilk_ws_broadcast_room(
            self._ctx, 
            room_name.encode('utf-8'), 
            message.encode('utf-8')
        )

    # SSE
    def sse_init(self):
        self._lib.csilk_sse_init(self._ctx)

    def sse_send(self, event, data):
        c_event = event.encode('utf-8') if event else None
        c_data = data.encode('utf-8') if data else None
        self._lib.csilk_sse_send(self._ctx, c_event, c_data)

    def sse_close(self):
        self._lib.csilk_sse_close(self._ctx)

    # Streaming and H2 Push
    def response_write(self, data):
        self.write(data)

        if not getattr(self, '_response_ended', False) and not self._lib.csilk_is_async(self._ctx):
            self.response_end()

    def dispatch_async(self, coro, loop):
        """
        Execute an asyncio coroutine and dispatch the response sending back to
        the main C event loop thread safely.
        """
        import asyncio
        import traceback

        # Ensure async mode is set so C loop doesn't automatically close context
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
        func_id = id(func)
        _dispatcher_map[func_id] = func
        self._lib.csilk_dispatch(self._ctx, _python_dispatcher, func_id)

    def response_end(self):
        if not getattr(self, '_response_ended', False):
            self._response_ended = True
            self._lib.csilk_response_end(self._ctx)

    def push_promise(self, method, path):
        return self._lib.csilk_push_promise(
            self._ctx,
            method.encode('utf-8'),
            path.encode('utf-8')
        )

    # Cookies
    def get_cookie(self, name):
        res = self._lib.csilk_get_cookie(self._ctx, name.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def set_cookie(self, name, value, max_age=0, path=None, domain=None, secure=False, http_only=False):
        self._lib.csilk_set_cookie(
            self._ctx,
            name.encode('utf-8'),
            value.encode('utf-8'),
            max_age,
            path.encode('utf-8') if path else None,
            domain.encode('utf-8') if domain else None,
            1 if secure else 0,
            1 if http_only else 0
        )

    # Form urlencoded parsing
    def parse_form_urlencoded(self):
        self._lib.csilk_parse_form_urlencoded(self._ctx)

    # Hash map properties (Iterated from C)
    @property
    def headers(self):
        from csilk.lib import CsilkHeaderCb
        h_dict = CaseInsensitiveDict()
        @CsilkHeaderCb
        def cb(key, value, arg):
            h_dict[key.decode('latin-1')] = value.decode('latin-1')
            return 1
        self._lib.csilk_for_each_header(self._ctx, cb, None)
        return h_dict

    @property
    def queries(self):
        from csilk.lib import CsilkHeaderCb
        q_dict = {}
        @CsilkHeaderCb
        def cb(key, value, arg):
            q_dict[key.decode('latin-1')] = value.decode('latin-1')
            return 1
        self._lib.csilk_for_each_query(self._ctx, cb, None)
        return q_dict

    @property
    def form_fields(self):
        from csilk.lib import CsilkHeaderCb
        f_dict = {}
        @CsilkHeaderCb
        def cb(key, value, arg):
            f_dict[key.decode('latin-1')] = value.decode('latin-1')
            return 1
        self._lib.csilk_for_each_form_field(self._ctx, cb, None)
        return f_dict

    # Handler metadata
    @property
    def handler_path(self):
        res = self._lib.csilk_ctx_get_handler_path(self._ctx)
        return res.decode('utf-8') if res else None

    @property
    def handler_perm_required(self):
        res = self._lib.csilk_ctx_get_handler_perm_required(self._ctx)
        return res.decode('utf-8') if res else None

    @property
    def handler_perm_resource(self):
        res = self._lib.csilk_ctx_get_handler_perm_resource(self._ctx)
        return res.decode('utf-8') if res else None

    # Storage functions
    def set_string(self, key, value, ttl_sec=0):
        return self._lib.csilk_set_string(self._ctx, key.encode('utf-8'), value.encode('utf-8'), ttl_sec)

    def get_string(self, key):
        res = self._lib.csilk_get_string(self._ctx, key.encode('utf-8'))
        if res:
            val = ctypes.string_at(res).decode('utf-8')
            self._lib.csilk_free(res)
            return val
        return None

    def incr(self, key, ttl_sec=0):
        return self._lib.csilk_incr(self._ctx, key.encode('utf-8'), ttl_sec)

    # Response body mutation
    @property
    def response_body(self):
        out_len = ctypes.c_size_t(0)
        res = self._lib.csilk_get_response_body(self._ctx, ctypes.byref(out_len))
        if res and out_len.value > 0:
            return ctypes.string_at(res, out_len.value)
        return b""

    @response_body.setter
    def response_body(self, body):
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
            
        self._lib.csilk_set_response_body(
            self._ctx,
            c_ptr,
            size,
            0
        )

    # Session Management
    @property
    def session(self):
        if not hasattr(self, "_session_obj"):
            self._session_obj = Session(self)
        return self._session_obj

    def session_start(self):
        self._lib.csilk_session_start(self._ctx)

    def session_destroy(self):
        sid = self.session_id
        if sid:
            _python_sessions.pop(sid, None)
        self._lib.csilk_session_destroy(self._ctx)

    @property
    def session_id(self):
        res = self._lib.csilk_session_get_id(self._ctx)
        return res.decode('utf-8') if res else None

    # JWT Helper Properties and Methods
    @property
    def jwt_payload(self):
        if not hasattr(self, "_jwt_payload"):
            res = self._lib.csilk_ctx_get_jwt_payload_json(self._ctx)
            if res:
                val = ctypes.string_at(res).decode('utf-8')
                self._lib.csilk_free(res)
                self._jwt_payload = json.loads(val)
            else:
                self._jwt_payload = None
        return self._jwt_payload

    def jwt_generate(self, payload, secret):
        payload_json = json.dumps(payload).encode('utf-8')
        c_secret = secret.encode('utf-8')
        res = self._lib.csilk_jwt_generate_json(self._ctx, payload_json, c_secret)
        if res:
            token = ctypes.string_at(res).decode('utf-8')
            self._lib.csilk_free(res)
            return token
        return None

    # Request-local context storage (opaque pointers)
    def set(self, key, value):
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

    def get(self, key):
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

    @classmethod
    def create_test_context(cls):
        lib = get_bindings()
        ctx_ptr = lib.csilk_test_ctx_new()
        if not ctx_ptr:
            raise RuntimeError("Failed to create test context")
        ctx = cls(ctx_ptr)
        ctx._owned = True
        return ctx

    def free_test_context(self):
        if hasattr(self, "_owned") and self._owned and self._ctx:
            self._lib.csilk_test_ctx_free(self._ctx)
            self._ctx = None
            self._owned = False

    def multipart_parse(self, handler):
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
                    "data": data_bytes
                }
                handler(part_dict)
            except Exception as e:
                import traceback
                traceback.print_exc()

        self._lib.csilk_multipart_parse(self._ctx, wrapper)


