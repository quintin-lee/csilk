class Csilk < Formula
  desc "Lightweight HTTP web framework written in C23 for high-concurrency microservices"
  homepage "https://github.com/quintin-lee/csilk"
  url "https://github.com/quintin-lee/csilk/archive/refs/tags/v0.9.0.tar.gz"
  license "MIT"
  head "https://github.com/quintin-lee/csilk.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "pkg-config" => :build
  depends_on "curl"
  depends_on "libuv"
  depends_on "libyaml"
  depends_on "openssl@3"
  depends_on "sqlite"
  depends_on "zlib"

  def install
    args = %w[
      -DCMAKE_BUILD_TYPE=Release
      -DCSILK_BUILD_SHARED=ON
      -DBUILD_TESTING=OFF
      -DBUILD_EXAMPLES=OFF
    ]

    system "cmake", "-S", ".", "-B", "build", *args, *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"test.c").write <<~C
      #include <stdio.h>
      #include "csilk/csilk.h"
      int main(void) {
          csilk_arena_t *a = csilk_arena_new(0);
          if (!a) return 1;
          void *mem = csilk_arena_alloc(a, 64);
          csilk_arena_free(a);
          return mem ? 0 : 1;
      }
    C
    system ENV.cc, "test.c", "-I#{include}", "-L#{lib}", "-lcsilk", "-o", "test_csilk"
    system "./test_csilk"
  end
end
