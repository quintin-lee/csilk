"""Cryptographic utilities — secure random bytes, UUID, and CSRF token generation.

Wraps the underlying csilk C crypto primitives for generating
cryptographically strong random values.
"""

import ctypes
from csilk.lib import get_bindings


class Crypto:
    """Collection of static cryptographic helper methods.

    All methods are stateless and thread-safe.
    """

    @staticmethod
    def random_bytes(length: int) -> bytes:
        """Generate *length* cryptographically secure random bytes.

        Args:
            length: Number of random bytes to generate.

        Returns:
            Raw bytes containing the random data.

        Raises:
            RuntimeError: If the underlying C call fails.
        """
        lib = get_bindings()
        buf = ctypes.create_string_buffer(length)
        res = lib.csilk_crypto_fill_random(buf, length)
        if res != 0:
            raise RuntimeError("Failed to generate cryptographically secure random bytes")
        return buf.raw

    @staticmethod
    def generate_uuid() -> str:
        """Generate a version-4 UUID string (e.g. ``"550e8400-..."``).

        Returns:
            36-character UUID string (plus null terminator).
        """
        lib = get_bindings()
        buf = ctypes.create_string_buffer(37)
        lib.csilk_generate_uuid(buf)
        return buf.value.decode('utf-8')

    @staticmethod
    def generate_csrf_token() -> str:
        """Generate a hex-encoded CSRF token suitable for form validation.

        Returns:
            32-character hex string.
        Raises:
            RuntimeError: If token generation fails.
        """
        lib = get_bindings()
        buf = ctypes.create_string_buffer(33)
        res = lib.csilk_csrf_generate_token(buf, 33)
        if res != 0:
            raise RuntimeError("Failed to generate CSRF token")
        return buf.value.decode('utf-8')
