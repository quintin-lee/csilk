# macOS Platform Support Assessment

> Date: 2026-05-30 | CI attempt: 2026-05-30 (5 rounds)

## Background

macOS CI (macos-14 ARM64) was attempted and uncovered 4 categories of issues.
This document evaluates feasibility and proposes a plan.

## Issues Found

### 1. Compiler: Apple Clang lacks C23

| Feature | Status |
|---------|:------:|
| `static constexpr` | Not supported |
| `nullptr` keyword | Not supported |
| `bool` keyword | Not supported |

**Fix**: Use Homebrew LLVM 19+ (`brew install llvm`). Works for compilation
but adds complexity to CI and user build instructions.

### 2. cJSON C23 Keyword Conflicts

cJSON v1.7.18 defines `true`/`false` macros that conflict with C23 keywords
when compiled with clang in C23 mode.

**Fix**: Add `-Wno-keyword-macro` to CMAKE_C_FLAGS. Works on all platforms.

### 3. Apple SDK Deprecation Warnings

macOS SDK deprecates `sprintf` — cJSON uses it. Promoted to error with
`-Werror,-Wdeprecated-declarations`.

**Fix**: Add `-Wno-deprecated-declarations` to CMAKE_C_FLAGS.

### 4. `pthread_barrier_t` Not Available (BLOCKER)

macOS does not implement `pthread_barrier_t` (POSIX optional feature).
csilk uses it in `src/core/server.c` for multi-worker thread synchronization.

**Impact**: Multi-worker mode (`worker_threads > 1`) cannot work on macOS.

## Solutions for pthread_barrier_t

| Approach | Effort | Risk |
|----------|:------:|:----:|
| **A:** Use `pthread_mutex` + `pthread_cond` replacement | 2-3 days | Low |
| **B:** Use `os_unfair_lock` + `dispatch_semaphore` (Darwin-only) | 1-2 days | Medium — non-portable |
| **C:** Restrict macOS to single-worker mode only | 1 hour | Low — limited functionality |
| **D:** Use libuv barrier equivalent (`uv_barrier_t`) | 1 day | Low — libuv provides cross-platform barrier |

## Recommendation: Approach D (uv_barrier_t)

libuv 1.45+ provides `uv_barrier_t` / `uv_barrier_init` / `uv_barrier_wait` /
`uv_barrier_destroy`. Since csilk already depends on libuv v1.48, this is the
most portable and maintainable solution.

**Implementation**:
1. Replace `pthread_barrier_t` with `uv_barrier_t`
2. Replace `pthread_barrier_init` → `uv_barrier_init`
3. Replace `pthread_barrier_wait` → `uv_barrier_wait`
4. Replace `pthread_barrier_destroy` → `uv_barrier_destroy`

**Files affected**: `src/core/server.c` (3 locations around line 589, 670, 800)

## Other Considerations

### Homebrew Path Differences (Intel vs Apple Silicon)

- Intel: `/usr/local/`
- Apple Silicon: `/opt/homebrew/`

CMakeLists.txt `find_library` handles this, but CI must set
`CMAKE_PREFIX_PATH` conditionally.

### CI Matrix Recommendation

Once pthread_barrier_t is replaced with uv_barrier_t, add macOS to CI:
```yaml
os: [ubuntu-24.04, macos-14]
# Ubuntu uses GCC (default), macOS uses Homebrew LLVM
```

## Verdict

**Implementable with ~2-3 days of work**. Primary blocker is `pthread_barrier_t`
which has a clean fix using `uv_barrier_t`. Recommend targeting v0.5.0 for macOS
support.
