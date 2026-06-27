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
            app.free()
            t.join(timeout=2)

if __name__ == '__main__':
    import gc, sys
    result = unittest.main(exit=False)
    gc.collect()
    sys.exit(not result.result.wasSuccessful())
