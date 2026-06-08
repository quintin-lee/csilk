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
