class Csilk < Formula
  desc "Lightweight HTTP web framework written in C23 for high-concurrency microservices"
  homepage "https://github.com/quintin-lee/csilk"
  url "https://github.com/quintin-lee/csilk/archive/refs/tags/v1.3.0.tar.gz"
  license "MIT"
  head "https://github.com/quintin-lee/csilk.git", branch: "master"

  option "with-full", "Build all optional domain modules (requires curl and sqlite)"
  option "with-db", "Build database module (requires sqlite)"
  option "with-ai", "Build AI module (requires curl)"

  depends_on "cmake" => :build
  depends_on "pkg-config" => :build
  depends_on "libuv"
  depends_on "libyaml"
  depends_on "openssl@3"
  depends_on "zlib"
  depends_on "curl" => :optional
  depends_on "sqlite" => :optional

  def install
    args = %w[
      -DCMAKE_BUILD_TYPE=Release
      -DCSILK_BUILD_SHARED=ON
      -DCSILK_BUILD_TESTS=OFF
      -DCSILK_BUILD_EXAMPLES=OFF
    ]

    if build.with? "full"
      args << "-DCSILK_BUILD_FULL=ON"
    else
      args << "-DCSILK_BUILD_FULL=OFF"
      args << "-DCSILK_ENABLE_DB=ON" if build.with?("db") || build.with?("sqlite")
      args << "-DCSILK_ENABLE_AI=ON" if build.with?("ai") || build.with?("curl")
    end

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
