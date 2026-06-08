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
