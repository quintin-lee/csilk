import os
import sys
import subprocess
import platform
import shutil
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext

class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=""):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)

class CMakeBuild(build_ext):
    def run(self):
        try:
            subprocess.check_output(["cmake", "--version"])
        except OSError:
            raise RuntimeError("CMake must be installed to build the following extensions: " +
                               ", ".join(e.name for e in self.extensions))
        
        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))
        
        # Determine the target directory for the shared library
        # We want to put it in the csilk package folder so it can be distributed
        csilk_pkg_dir = os.path.join(extdir, "csilk")
        os.makedirs(csilk_pkg_dir, exist_ok=True)
        
        cfg = "Debug" if self.debug else "Release"
        
        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={csilk_pkg_dir}",
            f"-DCMAKE_BUILD_TYPE={cfg}",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DCSILK_BUILD_TESTS=OFF",
            "-DCSILK_BUILD_EXAMPLES=OFF",
            "-DCSILK_BUILD_FUZZERS=OFF",
            # Ensure it is built as a shared library
            "-DBUILD_SHARED_LIBS=ON"
        ]

        if platform.system() == "Darwin":
            brew_prefix = subprocess.check_output(["brew", "--prefix"], text=True).strip()
            cmake_args.append(f"-DCMAKE_PREFIX_PATH={brew_prefix};{brew_prefix}/opt/openssl;{brew_prefix}/opt/libyaml;{brew_prefix}/opt/zlib;{brew_prefix}/opt/curl;{brew_prefix}/opt/sqlite3")
        
        build_args = ["--config", cfg, "--parallel"]

        if platform.system() == "Windows":
            cmake_args += [f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{cfg.upper()}={csilk_pkg_dir}"]
            cmake_args += [f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_{cfg.upper()}={csilk_pkg_dir}"]
            # For MSVC, runtime outputs (DLLs) go to RUNTIME_OUTPUT_DIRECTORY
            if sys.maxsize > 2**32:
                cmake_args += ["-A", "x64"]
        
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
            
        print(f"[csilk] cmake configure: cmake {' '.join(cmake_args)} {ext.sourcedir}", flush=True)
        result = subprocess.run(
            ["cmake"] + cmake_args + [ext.sourcedir],
            cwd=self.build_temp,
            capture_output=True,
            text=True
        )
        log_path = os.path.join(self.build_temp, "cmake_configure.log")
        with open(log_path, "w") as f:
            f.write("=== STDOUT ===\n")
            f.write(result.stdout or "")
            f.write("\n=== STDERR ===\n")
            f.write(result.stderr or "")
            f.write(f"\n=== RETURN CODE: {result.returncode} ===\n")
        print(f"[csilk] cmake configure output:\n{result.stdout}\n{result.stderr}", flush=True)
        result.check_returncode()
        
        print(f"[csilk] cmake build: cmake --build . {' '.join(build_args)}", flush=True)
        sys.stderr.write(f"[csilk] cmake build: cmake --build . {' '.join(build_args)}\n")
        sys.stderr.flush()
        result2 = subprocess.run(
            ["cmake", "--build", "."] + build_args,
            cwd=self.build_temp,
            capture_output=True,
            text=True
        )
        sys.stderr.write(result2.stdout)
        sys.stderr.write(result2.stderr)
        sys.stderr.flush()
        result2.check_returncode()

# Read version from CMake-generated version file (single source of truth)
_version_py = os.path.join(os.path.dirname(__file__), "python", "csilk", "_version.py")
if os.path.exists(_version_py):
    with open(_version_py) as f:
        __version__ = f.read().split("=")[1].strip().strip('"')
else:
    __version__ = "0.0.0"

setup(
    name="csilk",
    version=__version__,
    author="csilk contributors",
    description="High-performance asynchronous web framework in C and Python.",
    long_description=open("README.md", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    ext_modules=[CMakeExtension("csilk._dummy_ext")],
    cmdclass={"build_ext": CMakeBuild},
    package_dir={"": "python"},
    packages=find_packages(where="python"),
    include_package_data=True,
    python_requires=">=3.7",
    entry_points={
        "console_scripts": [
            "csilk=csilk.cli:main"
        ]
    }
)
