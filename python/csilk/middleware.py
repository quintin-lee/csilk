"""Middleware wrappers for csilk framework.

Provides CircuitBreaker, SlidingLimiter, and OTLP Tracing wrappers.
"""

from csilk.lib import get_bindings, CsilkCircuitBreakerConfig

class CircuitBreaker:
    """High-level Python wrapper for csilk C Circuit Breaker."""

    def __init__(self, failure_threshold: int = 5, recovery_timeout_ms: int = 5000):
        self._lib = get_bindings()
        cfg = CsilkCircuitBreakerConfig(
            failure_threshold=failure_threshold,
            recovery_timeout_ms=recovery_timeout_ms
        )
        self._cb = self._lib.csilk_circuit_breaker_new(cfg)
        if not self._cb:
            raise RuntimeError("Failed to create Circuit Breaker")

    def middleware(self, ctx):
        """Invoke Circuit Breaker middleware on a request context."""
        self._lib.csilk_circuit_breaker_middleware(ctx._ctx, self._cb)

    def record_success(self):
        """Record downstream success."""
        self._lib.csilk_circuit_breaker_record_success(self._cb)

    def record_failure(self):
        """Record downstream failure."""
        self._lib.csilk_circuit_breaker_record_failure(self._cb)

    @property
    def state(self) -> int:
        """Get circuit breaker state: 0=CLOSED, 1=OPEN, 2=HALF_OPEN."""
        return self._lib.csilk_circuit_breaker_get_state(self._cb)

    def __del__(self):
        if hasattr(self, "_cb") and self._cb:
            self._lib.csilk_circuit_breaker_free(self._cb)
            self._cb = None


class SlidingLimiter:
    """High-level Python wrapper for csilk C Sliding Window Rate Limiter."""

    def __init__(self, limit_per_window: int = 60, window_ms: int = 60000):
        self._lib = get_bindings()
        self._limiter = self._lib.csilk_sliding_limiter_new(limit_per_window, window_ms)
        if not self._limiter:
            raise RuntimeError("Failed to create Sliding Limiter")

    def middleware(self, ctx):
        """Invoke Sliding Window Rate Limiter middleware on a request context."""
        self._lib.csilk_sliding_rate_limit_middleware(ctx._ctx, self._limiter)

    def __del__(self):
        if hasattr(self, "_limiter") and self._limiter:
            self._lib.csilk_sliding_limiter_free(self._limiter)
            self._limiter = None


def trace_middleware(ctx):
    """Invoke OpenTelemetry W3C Trace Context middleware on a request context."""
    lib = get_bindings()
    lib.csilk_trace_middleware(ctx._ctx)
