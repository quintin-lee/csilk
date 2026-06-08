import ctypes
import json
from csilk.lib import get_bindings

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
            return res[:out_len.value]
        return b""

    def query(self, key):
        res = self._lib.csilk_get_query(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def param(self, key):
        res = self._lib.csilk_get_param(self._ctx, key.encode('utf-8'))
        return res.decode('utf-8') if res else None

    def status(self, code):
        self._lib.csilk_status(self._ctx, code)

    def string(self, code, text):
        self._lib.csilk_string(self._ctx, code, text.encode('utf-8'))

    def json(self, code, data):
        json_str = json.dumps(data)
        self.set_header("Content-Type", "application/json")
        self._lib.csilk_string(self._ctx, code, json_str.encode('utf-8'))

    def set_header(self, key, value):
        self._lib.csilk_set_header(self._ctx, key.encode('utf-8'), value.encode('utf-8'))

    def file(self, path):
        self._lib.csilk_file(self._ctx, path.encode('utf-8'))

    def redirect(self, code, location):
        self._lib.csilk_redirect(self._ctx, code, location.encode('utf-8'))
