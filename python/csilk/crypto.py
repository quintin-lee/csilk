import ctypes
from csilk.lib import get_bindings

class Crypto:
    @staticmethod
    def random_bytes(length):
        lib = get_bindings()
        buf = ctypes.create_string_buffer(length)
        res = lib.csilk_crypto_fill_random(buf, length)
        if res != 0:
            raise RuntimeError("Failed to generate cryptographically secure random bytes")
        return buf.raw

    @staticmethod
    def generate_uuid():
        lib = get_bindings()
        buf = ctypes.create_string_buffer(37)
        lib.csilk_generate_uuid(buf)
        return buf.value.decode('utf-8')
