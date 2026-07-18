"""Unit tests for Python C-ABI bindings for OTLP Trace, Circuit Breaker, and Sliding Limiter."""

from csilk.context import Context
from csilk.middleware import CircuitBreaker, SlidingLimiter, trace_middleware

def test_trace_middleware_binding():
    print("Testing Python trace_middleware binding...")
    ctx = Context.create_test_context()
    try:
        trace_middleware(ctx)
        tid = ctx.trace_id
        sid = ctx.span_id
        assert tid is not None, "trace_id is None"
        assert sid is not None, "span_id is None"
        assert len(tid) == 32, f"Invalid trace_id length: {len(tid)}"
        assert len(sid) == 16, f"Invalid span_id length: {len(sid)}"
        print(f"PASS: trace_id={tid}, span_id={sid}")
    finally:
        ctx.free_test_context()

def test_circuit_breaker_binding():
    print("Testing Python CircuitBreaker binding...")
    cb = CircuitBreaker(failure_threshold=2, recovery_timeout_ms=1000)
    assert cb.state == 0
    cb.record_failure()
    assert cb.state == 0
    cb.record_failure()
    assert cb.state == 1
    print("PASS: CircuitBreaker state transitions")

def test_sliding_limiter_binding():
    print("Testing Python SlidingLimiter binding...")
    lim = SlidingLimiter(limit_per_window=2, window_ms=1000)
    ctx1 = Context.create_test_context()
    ctx2 = Context.create_test_context()
    ctx3 = Context.create_test_context()
    try:
        lim.middleware(ctx1)
        assert ctx1.status_code != 429

        lim.middleware(ctx2)
        assert ctx2.status_code != 429

        lim.middleware(ctx3)
        assert ctx3.status_code == 429
        print("PASS: SlidingLimiter rate limiting")
    finally:
        ctx1.free_test_context()
        ctx2.free_test_context()
        ctx3.free_test_context()

if __name__ == "__main__":
    test_trace_middleware_binding()
    test_circuit_breaker_binding()
    test_sliding_limiter_binding()
    print("All Python C-ABI binding tests passed successfully!")
