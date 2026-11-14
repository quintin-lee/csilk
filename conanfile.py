from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
import os

class CsilkConan(ConanFile):
    name = "csilk"
    version = "0.9.0"
    license = "MIT"
    author = "quintin-lee"
    url = "https://github.com/quintin-lee/csilk"
    description = "A lightweight HTTP web framework written in C23, delivering P99 latency <= 5ms under 10K QPS on commodity hardware."
    topics = ("http", "web", "server", "c23", "io_uring", "libuv", "rest", "api-gateway")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_uring": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_uring": False,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC
        if self.settings.os != "Linux":
            self.options.with_uring = False

    def requirements(self):
        self.requires("libuv/1.48.0")
        self.requires("openssl/3.3.1")
        self.requires("zlib/1.3.1")
        self.requires("libcurl/8.8.0")
        self.requires("libyaml/0.2.5")
        self.requires("sqlite3/3.46.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["CSILK_BUILD_SHARED"] = self.options.shared
        tc.variables["BUILD_TESTING"] = False
        tc.variables["BUILD_EXAMPLES"] = False
        if self.options.with_uring:
            tc.variables["CSILK_USE_URING"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["csilk"]
        self.cpp_info.set_property("cmake_file_name", "csilk")
        self.cpp_info.set_property("cmake_target_name", "csilk::csilk")
        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["pthread", "m", "dl"]
