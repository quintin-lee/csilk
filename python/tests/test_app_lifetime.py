import socket
import threading
import time
import unittest


def _wait_ready(port, timeout=5.0):
    """Block until a TCP server is accepting connections on `port`."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError(f"no server listening on port {port}")


class TestAppLifetime(unittest.TestCase):
    def test_free_stops_and_closes_event_loop(self):
        from csilk.app import App

        app = App()
        loop = app._loop
        app.free()

        self.assertTrue(loop.is_closed())
        self.assertFalse(app._loop_thread.is_alive())

    def test_free_with_groups_is_idempotent(self):
        from csilk.app import App

        app = App()
        app.group("/api")
        app.free()
        app.free()

        self.assertIsNone(app._app)

    def test_stop_before_run_is_safe(self):
        """Regression: stop() on a never-run server must not crash.

        csilk_server_stop used to unconditionally uv_async_send the server's
        async handle; on a server whose run() never executed the handle is
        zeroed (calloc'd) and uv_async_send segfaulted. The native side now
        ignores stop requests for handles that were never initialized or are
        already closing.
        """
        from csilk.app import App

        app = App()
        # Native server exists but csilk_server_run never ran:
        # its async handle was never initialized (zeroed by calloc).
        server = app._lib.csilk_app_server(app._app)
        self.assertIsNotNone(server)
        app.stop() # must not raise or crash
        t = threading.Thread(target=app.run, args=(8099,), daemon=True)
        t.start()
        _wait_ready(8099) # server actually bound and serving
        app.stop() # stop while running is the normal path
        t.join(timeout=2.0)
        self.assertFalse(t.is_alive(), "run thread did not exit after stop()")
        app.free()
        app.free() # idempotent


if __name__ == "__main__":
    unittest.main()
