import unittest
from csilk.app import App
from csilk.depends import Depends
from csilk.context import Context
import requests
import threading
import time

def get_db():
    return "DB"

def get_user(db = Depends(get_db)):
    return f"User from {db}"

class TestCsilkDepends(unittest.TestCase):
    def test_dependency_injection(self):
        app = App()
        
        @app.get("/test_di")
        def handle_di(ctx: Context, user = Depends(get_user)):
            ctx.json(200, {"user": user})
            
        t = threading.Thread(target=app.run, args=(8095,), daemon=True)
        t.start()
        time.sleep(1) # wait for server to start
        
        try:
            resp = requests.get("http://localhost:8095/test_di", proxies={"http": None, "https": None})
            self.assertEqual(resp.status_code, 200)
            self.assertEqual(resp.json(), {"user": "User from DB"})
        finally:
            app.stop()
            t.join(timeout=2)
            app.free()

    def test_missing_annotation(self):
        app = App()
        @app.get("/missing")
        def handle_missing(ctx: Context, bad_dep = Depends()):
            pass
            
        t = threading.Thread(target=app.run, args=(8097,), daemon=True)
        t.start()
        time.sleep(1) # wait for server to start
        
        try:
            resp = requests.get("http://localhost:8097/missing", proxies={"http": None, "https": None})
            self.assertEqual(resp.status_code, 500)
            self.assertIn("must be explicitly provided or type-annotated", resp.text)
        finally:
            app.stop()
            t.join(timeout=2)
            app.free()

    def test_async_dependency_in_sync_handler(self):
        app = App()
        
        async def async_dep():
            return "async data"
            
        @app.get("/sync_handler")
        def handle_sync(ctx: Context, data = Depends(async_dep)):
            ctx.string(200, data)
            
        t = threading.Thread(target=app.run, args=(8096,), daemon=True)
        t.start()
        time.sleep(1) # wait for server to start
        
        try:
            # We expect a 500 error because RuntimeError is raised
            resp = requests.get("http://localhost:8096/sync_handler", proxies={"http": None, "https": None})
            self.assertEqual(resp.status_code, 500)
        finally:
            app.stop()
            t.join(timeout=2)
            app.free()

if __name__ == '__main__':
    import gc, sys
    result = unittest.main(exit=False)
    gc.collect()
    sys.exit(not result.result.wasSuccessful())
