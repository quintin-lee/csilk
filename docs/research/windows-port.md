# Windows (MSVC) Support Assessment

> Date: 2026-05-30 | Status: Pre-research

## Summary

Windows support via MSVC is **not feasible in the short term** due to deep
POSIX API dependencies. Cross-compilation via MinGW or WSL2 is more practical.

## API Compatibility Matrix

| API/Dependency | Windows Support | Notes |
|---------------|:--------------:|-------|
| **libuv** | Full | First-class Windows support (IOCP backend) |
| **llhttp** | Full | Pure C, no POSIX dependency |
| **nghttp2** | Full | Windows build supported via CMake |
| **cJSON** | Full | Pure C, cross-platform |
| **pthread** | Via pthreads-win32 | Different API surface |
| **sys/socket.h** | Winsock2 | Different headers, types, constants |
| **unistd.h** | io.h / process.h | Different header mapping |
| **sendfile()** | TransmitFile() | Completely different API |
| **setjmp/longjmp** | Partial | Available but different semantics |
| **signal()** | Limited | No SIGPIPE, different signal model |
| **fork()** | None | Windows uses CreateProcess |
| **SO_REUSEPORT** | None | Windows uses SO_REUSEADDR differently |
| **mmap()** | VirtualAlloc/MapViewOfFile | Completely different API |
| **OpenSSL** | Full | Windows binaries available |
| **zlib** | Full | Windows support |
| **libcurl** | Full | Windows support |

## Impacted Source Files

| File | Issue |
|------|-------|
| `src/core/server.c` | `SO_REUSEPORT`, `pthread_barrier_t`, `fork()`, signal handling |
| `src/core/connection.c` | `accept()`, socket options, `sendfile()` |
| `src/core/tls.c` | BIO pairs, ALPN (works via OpenSSL) |
| `src/core/http1.c` | `writev()` → `WSASend()` |
| `src/core/logger.c` | ANSI color escape codes (Windows terminal) |
| `src/core/config.c` | File path handling, `realpath()` |
| `src/middleware/static.c` | `sendfile()` → `TransmitFile()` |
| `src/middleware/gzip.c` | zlib works, thread pool via libuv |

## Practical Alternatives

### Option A: MinGW (GCC on Windows)

- Compiles POSIX code with minimal changes
- libuv, OpenSSL, zlib all work
- pthreads available via winpthreads
- Effort: ~1 week for build system, ~2 weeks for fixes

### Option B: WSL2 (Linux on Windows)

- Zero code changes — runs as native Linux binary
- Best option for development
- Production deployment: containerized via Docker on Windows

### Option C: Full MSVC Port

- Requires `#ifdef _WIN32` guards throughout the codebase
- Replace all POSIX APIs with Windows equivalents
- Estimated effort: 8-12 weeks
- Ongoing maintenance burden

## Verdict

**Not recommended for v1.0**. The POSIX dependency surface is too large for
a practical MSVC port. Users on Windows should use WSL2 for development or
run csilk in Docker. MinGW cross-compilation is the most viable path if
native Windows binaries are needed — evaluate post-v1.0.
