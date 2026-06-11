import ctypes
import json
from csilk.lib import get_bindings

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
        out_len = ctypes.c_size_t(0)
        res = self._lib.csilk_get_body(self._ctx, ctypes.byref(out_len))
        if res and out_len.value > 0:
            return ctypes.string_at(res, out_len.value)
        return b""

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

    def string(self, code, text):
        self._lib.csilk_string(self._ctx, code, text.encode('utf-8'))

    def json(self, code, data):
        json_str = json.dumps(data)
        self.set_header("Content-Type", "application/json")
        self._lib.csilk_string(self._ctx, code, json_str.encode('utf-8'))

    def set_header(self, key, value):
        self._lib.csilk_set_header(
            self._ctx, 
            key.encode('utf-8'), 
            value.encode('utf-8') if value else None
        )

    def file(self, path):
        self._lib.csilk_file(self._ctx, path.encode('utf-8'))

    def redirect(self, code, location):
        self._lib.csilk_redirect(self._ctx, code, location.encode('utf-8'))

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
            h_dict[key.decode('utf-8')] = value.decode('utf-8')
            return 1
        self._lib.csilk_for_each_header(self._ctx, cb, None)
        return h_dict

    @property
    def queries(self):
        from csilk.lib import CsilkHeaderCb
        q_dict = {}
        @CsilkHeaderCb
        def cb(key, value, arg):
            q_dict[key.decode('utf-8')] = value.decode('utf-8')
            return 1
        self._lib.csilk_for_each_query(self._ctx, cb, None)
        return q_dict

    @property
    def form_fields(self):
        from csilk.lib import CsilkHeaderCb
        f_dict = {}
        @CsilkHeaderCb
        def cb(key, value, arg):
            f_dict[key.decode('utf-8')] = value.decode('utf-8')
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
        else:
            self._response_body_ref = bytes(body)
        self._lib.csilk_set_response_body(
            self._ctx,
            self._response_body_ref,
            len(self._response_body_ref),
            0
        )
