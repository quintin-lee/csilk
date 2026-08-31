import unittest


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


if __name__ == "__main__":
    unittest.main()
