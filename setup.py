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
        
        build_args = ["--config", cfg, "--parallel"]

        if platform.system() == "Windows":
            cmake_args += [f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{cfg.upper()}={csilk_pkg_dir}"]
            cmake_args += [f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_{cfg.upper()}={csilk_pkg_dir}"]
            # For MSVC, runtime outputs (DLLs) go to RUNTIME_OUTPUT_DIRECTORY
            if sys.maxsize > 2**32:
                cmake_args += ["-A", "x64"]
        
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
            
        subprocess.check_call(["cmake", ext.sourcedir] + cmake_args, cwd=self.build_temp)
        subprocess.check_call(["cmake", "--build", "."] + build_args, cwd=self.build_temp)

setup(
    name="csilk",
    version="0.5.0",
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
