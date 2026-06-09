# csilk Python Wrapper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a lightweight, high-performance Python wrapper for the `csilk` web framework using the standard library `ctypes` module.

**Architecture:** The wrapper consists of low-level `ctypes` definitions mapping C functions to Python, wrapped by developer-friendly `Context` and `App` classes. Python callback functions are wrapped into standard C function pointers (`csilk_handler_t`), with internal reference-tracking to prevent garbage collection crashes.

**Tech Stack:** Python 3 (standard library: `ctypes`, `unittest`, `urllib.request`), CMake, GCC/Clang.

---

## File Structure

- Create: `python/csilk/lib.py` (c_types wrappers and CDLL loader)
- Create: `python/csilk/context.py` (`Context` wrapper class)
- Create: `python/csilk/app.py` (`App` core class)
- Create: `python/csilk/__init__.py` (module exports)
- Create: `python/tests/test_csilk.py` (Python integration tests)

---

## Tasks

### Task 1: Setup Shared Library Build and Loader

**Files:**
- Modify: `CMakeLists.txt` (Build target settings)
- Create: `python/csilk/lib.py`
- Test: `python/tests/test_csilk.py`

- [ ] **Step 1: Write the failing test**

Create `python/tests/test_csilk.py`:
```python
import unittest
import os
import sys

# Add python path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

class TestCsilkLoader(unittest.TestCase):
    def test_library_load(self):
        from csilk.lib import load_lib
        lib = load_lib()
        self.assertIsNotNone(lib)

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: FAIL (ModuleNotFoundError: No module named 'csilk')

- [ ] **Step 3: Write minimal implementation**

Compile shared library:
```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DCSILK_BUILD_SHARED=ON -B build .
cmake --build build -j$(nproc)
```

Create `python/csilk/__init__.py`:
```python
# Empty init file to mark directory as Python package
```

Create `python/csilk/lib.py`:
```python
import ctypes
import os
import sys

def load_lib():
    env_path = os.environ.get("LIBCSILK_PATH")
    if env_path:
        return ctypes.CDLL(env_path)
    
    base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    lib_name = "libcsilk.so" if sys.platform != "darwin" else "libcsilk.dylib"
    build_path = os.path.join(base_dir, "build", lib_name)
    
    if os.path.exists(build_path):
        return ctypes.CDLL(build_path)
    
    try:
        return ctypes.CDLL(lib_name)
    except OSError as e:
        raise OSError(f"Could not load {lib_name}. Please compile csilk as shared library and configure LIBCSILK_PATH.") from e
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/csilk/__init__.py python/csilk/lib.py python/tests/test_csilk.py
git commit -m "feat(python): 📦 set up python package structure and ctypes shared library loader"
```

---

### Task 2: Low-Level ctypes Mappings

**Files:**
- Modify: `python/csilk/lib.py`
- Test: `python/tests/test_csilk.py`

- [ ] **Step 1: Write the failing test**

Modify `python/tests/test_csilk.py`:
```python
import unittest
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

class TestCsilkLoader(unittest.TestCase):
    def test_library_load(self):
        from csilk.lib import load_lib
        lib = load_lib()
        self.assertIsNotNone(lib)

    def test_ctypes_bindings(self):
        from csilk.lib import get_bindings
        lib = get_bindings()
        self.assertIsNotNone(lib.csilk_app_new)
        self.assertIsNotNone(lib.csilk_app_free)
        self.assertIsNotNone(lib.csilk_app_add_route)

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: FAIL (ImportError: cannot import name 'get_bindings')

- [ ] **Step 3: Write minimal implementation**

Modify `python/csilk/lib.py` to add `get_bindings`:
```python
import ctypes
import os
import sys

# Define opaque types
class CsilkApp(ctypes.Structure):
    pass
class CsilkCtx(ctypes.Structure):
    pass
class CsilkServer(ctypes.Structure):
    pass

CsilkAppPtr = ctypes.POINTER(CsilkApp)
CsilkCtxPtr = ctypes.POINTER(CsilkCtx)
CsilkServerPtr = ctypes.POINTER(CsilkServer)

# Handler function callback type
CsilkHandler = ctypes.CFUNCTYPE(None, CsilkCtxPtr)

_cached_lib = None

def load_lib():
    env_path = os.environ.get("LIBCSILK_PATH")
    if env_path:
        return ctypes.CDLL(env_path)
    
    base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    lib_name = "libcsilk.so" if sys.platform != "darwin" else "libcsilk.dylib"
    build_path = os.path.join(base_dir, "build", lib_name)
    
    if os.path.exists(build_path):
        return ctypes.CDLL(build_path)
    
    try:
        return ctypes.CDLL(lib_name)
    except OSError as e:
        raise OSError(f"Could not load {lib_name}. Please compile csilk as shared library and configure LIBCSILK_PATH.") from e

def get_bindings():
    global _cached_lib
    if _cached_lib is not None:
        return _cached_lib
    
    lib = load_lib()
    
    # App management
    lib.csilk_app_new.restype = CsilkAppPtr
    lib.csilk_app_new.argtypes = [ctypes.c_char_p]
    
    lib.csilk_app_free.restype = None
    lib.csilk_app_free.argtypes = [CsilkAppPtr]
    
    lib.csilk_app_add_route.restype = None
    lib.csilk_app_add_route.argtypes = [CsilkAppPtr, ctypes.c_char_p, ctypes.c_char_p, CsilkHandler]
    
    lib.csilk_app_run.restype = ctypes.c_int
    lib.csilk_app_run.argtypes = [CsilkAppPtr, ctypes.c_int]
    
    lib.csilk_app_server.restype = CsilkServerPtr
    lib.csilk_app_server.argtypes = [CsilkAppPtr]
    
    # Server management
    lib.csilk_server_stop.restype = None
    lib.csilk_server_stop.argtypes = [CsilkServerPtr]
    
    # Context getters
    lib.csilk_get_method.restype = ctypes.c_char_p
    lib.csilk_get_method.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_path.restype = ctypes.c_char_p
    lib.csilk_get_path.argtypes = [CsilkCtxPtr]
    
    lib.csilk_get_body.restype = ctypes.c_char_p
    lib.csilk_get_body.argtypes = [CsilkCtxPtr, ctypes.POINTER(ctypes.c_size_t)]
    
    lib.csilk_get_query.restype = ctypes.c_char_p
    lib.csilk_get_query.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_get_param.restype = ctypes.c_char_p
    lib.csilk_get_param.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    # Response helpers
    lib.csilk_status.restype = None
    lib.csilk_status.argtypes = [CsilkCtxPtr, ctypes.c_int]
    
    lib.csilk_string.restype = None
    lib.csilk_string.argtypes = [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]
    
    lib.csilk_set_header.restype = None
    lib.csilk_set_header.argtypes = [CsilkCtxPtr, ctypes.c_char_p, ctypes.c_char_p]
    
    lib.csilk_file.restype = None
    lib.csilk_file.argtypes = [CsilkCtxPtr, ctypes.c_char_p]
    
    lib.csilk_redirect.restype = None
    lib.csilk_redirect.argtypes = [CsilkCtxPtr, ctypes.c_int, ctypes.c_char_p]
    
    _cached_lib = lib
    return lib
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/csilk/lib.py python/tests/test_csilk.py
git commit -m "feat(python): 📦 register core ctypes bindings and structure types"
```

---

### Task 3: Context Wrapper Class

**Files:**
- Create: `python/csilk/context.py`
- Test: `python/tests/test_csilk.py`

- [ ] **Step 1: Write the failing test**

Modify `python/tests/test_csilk.py`:
```python
import unittest
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

class TestCsilkLoader(unittest.TestCase):
    def test_context_class_existence(self):
        from csilk.context import Context
        # Ensure Context constructor takes a ctypes pointer
        from csilk.lib import CsilkCtxPtr
        ctx = Context(CsilkCtxPtr())
        self.assertIsNotNone(ctx)

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: FAIL (ModuleNotFoundError: No module named 'csilk.context')

- [ ] **Step 3: Write minimal implementation**

Create `python/csilk/context.py`:
```python
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
```

Modify `python/csilk/__init__.py` to export Context:
```python
from csilk.context import Context
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/csilk/context.py python/csilk/__init__.py python/tests/test_csilk.py
git commit -m "feat(python): ✨ implement high-level Context wrapper class for request/response handling"
```

---

### Task 4: App Wrapper Class

**Files:**
- Create: `python/csilk/app.py`
- Modify: `python/csilk/__init__.py`
- Test: `python/tests/test_csilk.py`

- [ ] **Step 1: Write the failing test**

Modify `python/tests/test_csilk.py`:
```python
import unittest
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

class TestCsilkLoader(unittest.TestCase):
    def test_app_instantiation(self):
        from csilk.app import App
        app = App()
        self.assertIsNotNone(app)
        app.free()

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: FAIL (ModuleNotFoundError: No module named 'csilk.app')

- [ ] **Step 3: Write minimal implementation**

Create `python/csilk/app.py`:
```python
import ctypes
from csilk.lib import get_bindings, CsilkHandler
from csilk.context import Context

class App:
    def __init__(self, config_path=None):
        self._lib = get_bindings()
        c_config = config_path.encode('utf-8') if config_path else None
        self._app = self._lib.csilk_app_new(c_config)
        if not self._app:
            raise RuntimeError("Failed to create csilk application instance")
        self._handlers = [] # Reference storage to prevent garbage collection of callbacks

    def free(self):
        if self._app:
            self._lib.csilk_app_free(self._app)
            self._app = None
        self._handlers.clear()

    def route(self, method, path, handler):
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
                ctx.string(500, f"Internal Server Error: {str(e)}")

        self._handlers.append(wrapper)
        self._lib.csilk_app_add_route(
            self._app,
            method.encode('utf-8'),
            path.encode('utf-8'),
            wrapper
        )

    def get(self, path, handler):
        self.route("GET", path, handler)

    def post(self, path, handler):
        self.route("POST", path, handler)

    def run(self, port):
        return self._lib.csilk_app_run(self._app, port)

    def stop(self):
        server = self._lib.csilk_app_server(self._app)
        if server:
            self._lib.csilk_server_stop(server)

    def __del__(self):
        self.free()
```

Modify `python/csilk/__init__.py` to export App:
```python
from csilk.context import Context
from csilk.app import App
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest python/tests/test_csilk.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/csilk/app.py python/csilk/__init__.py python/tests/test_csilk.py
git commit -m "feat(python): ✨ implement App wrapper class with route routing and handler reference protection"
```

---

### Task 5: Integration Testing

**Files:**
- Modify: `python/tests/test_csilk.py`

- [ ] **Step 1: Write the failing test**

Modify `python/tests/test_csilk.py` to add server integration test:
```python
import unittest
import os
import sys
import threading
import time
import urllib.request
import json

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from csilk import App, Context

class TestCsilkIntegration(unittest.TestCase):
    def test_routing_and_requests(self):
        app = App()
        
        @app.get("/hello")
        def handle_hello(ctx: Context):
            ctx.string(200, "hello world from python")

        @app.post("/echo")
        def handle_echo(ctx: Context):
            req_body = ctx.body.decode('utf-8')
            data = json.loads(req_body)
            ctx.json(200, {"echoed": data})

        # Run server in thread
        def run_server():
            app.run(8081)
            
        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()
        
        # Give server time to spin up
        time.sleep(0.3)
        
        try:
            # 1. Test GET request
            res = urllib.request.urlopen("http://127.0.0.1:8081/hello")
            self.assertEqual(res.status, 200)
            self.assertEqual(res.read().decode('utf-8'), "hello world from python")

            # 2. Test POST JSON request
            req = urllib.request.Request(
                "http://127.0.0.1:8081/echo",
                data=json.dumps({"test": "value"}).encode('utf-8'),
                headers={"Content-Type": "application/json"}
            )
            res2 = urllib.request.urlopen(req)
            self.assertEqual(res2.status, 200)
            res_data = json.loads(res2.read().decode('utf-8'))
            self.assertEqual(res_data["echoed"]["test"], "value")
        finally:
            app.stop()
            t.join(timeout=1.0)

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest python/tests/test_csilk.py::TestCsilkIntegration`
Expected: FAIL (since the `@app.get` syntactic sugar/decorator syntax is not defined on the `App` class)

- [ ] **Step 3: Write minimal implementation**

Modify `python/csilk/app.py` to allow GET/POST routes to be used as decorators if called with a single argument (or add a separate routing decorator helper):
```python
import ctypes
from csilk.lib import get_bindings, CsilkHandler
from csilk.context import Context

class App:
    def __init__(self, config_path=None):
        self._lib = get_bindings()
        c_config = config_path.encode('utf-8') if config_path else None
        self._app = self._lib.csilk_app_new(c_config)
        if not self._app:
            raise RuntimeError("Failed to create csilk application instance")
        self._handlers = []

    def free(self):
        if self._app:
            self._lib.csilk_app_free(self._app)
            self._app = None
        self._handlers.clear()

    def route(self, method, path, handler):
        @CsilkHandler
        def wrapper(ctx_ptr):
            ctx = Context(ctx_ptr)
            try:
                handler(ctx)
            except Exception as e:
                import traceback
                traceback.print_exc()
                ctx.string(500, f"Internal Server Error: {str(e)}")

        self._handlers.append(wrapper)
        self._lib.csilk_app_add_route(
            self._app,
            method.encode('utf-8'),
            path.encode('utf-8'),
            wrapper
        )

    def get(self, path, handler=None):
        if handler is None:
            def decorator(h):
                self.route("GET", path, h)
                return h
            return decorator
        self.route("GET", path, handler)

    def post(self, path, handler=None):
        if handler is None:
            def decorator(h):
                self.route("POST", path, h)
                return h
            return decorator
        self.route("POST", path, handler)

    def run(self, port):
        return self._lib.csilk_app_run(self._app, port)

    def stop(self):
        server = self._lib.csilk_app_server(self._app)
        if server:
            self._lib.csilk_server_stop(server)

    def __del__(self):
        self.free()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest python/tests/test_csilk.py::TestCsilkIntegration`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/csilk/app.py python/tests/test_csilk.py
git commit -m "test(python): ✅ add decorator routing support and verify integration server requests"
```
