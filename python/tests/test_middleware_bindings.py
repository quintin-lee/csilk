"""Unit tests for Python C-ABI bindings for OTLP Trace, Circuit Breaker, and Sliding Limiter."""

import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from csilk.context import Context
from csilk.middleware import CircuitBreaker, SlidingLimiter, trace_middleware


class TestMiddlewareBindings(unittest.TestCase):
    def test_trace_middleware_binding(self):
        ctx = Context.create_test_context()
        try:
            trace_middleware(ctx)
            tid = ctx.trace_id
            sid = ctx.span_id
            self.assertIsNotNone(tid, "trace_id is None")
            self.assertIsNotNone(sid, "span_id is None")
            self.assertEqual(len(tid), 32, f"Invalid trace_id length: {len(tid)}")
            self.assertEqual(len(sid), 16, f"Invalid span_id length: {len(sid)}")
        finally:
            ctx.free_test_context()

    def test_circuit_breaker_binding(self):
        cb = CircuitBreaker(failure_threshold=2, recovery_timeout_ms=1000)
        self.assertEqual(cb.state, 0)
        cb.record_failure()
        self.assertEqual(cb.state, 0)
        cb.record_failure()
        self.assertEqual(cb.state, 1)

    def test_sliding_limiter_binding(self):
        lim = SlidingLimiter(limit_per_window=2, window_ms=1000)
        ctx1 = Context.create_test_context()
        ctx2 = Context.create_test_context()
        ctx3 = Context.create_test_context()
        try:
            lim.middleware(ctx1)
            self.assertNotEqual(ctx1.status_code, 429)

            lim.middleware(ctx2)
            self.assertNotEqual(ctx2.status_code, 429)

            lim.middleware(ctx3)
            self.assertEqual(ctx3.status_code, 429)
        finally:
            ctx1.free_test_context()
            ctx2.free_test_context()
            ctx3.free_test_context()


if __name__ == "__main__":
    unittest.main()
