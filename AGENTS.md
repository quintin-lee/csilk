# AGENTS.md — csilk

## Build & Test

```bash
# Configure (default: Release, clang)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang

# Build everything
cmake --build build -j$(nproc)

# Run unit tests (excludes integration)
ctest --test-dir build -E test_integration --output-on-failure

# Run a single test
ctest --test-dir build -R test_bcrypt --output-on-failure

# Run integration tests separately
ctest --test-dir build -R test_integration --output-on-failure
```

**CI matrix**: `ubuntu-24.04` + `macos-14`, `Debug` + `Release`. ASAN on Linux Debug, TSAN separate job, coverage uses **gcc** (not clang) with `-DENABLE_OOM_TEST=OFF`.

## Sanitizer Builds

```bash
# ASAN (address + leak)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_ASAN=ON -DENABLE_OOM_TEST=ON -DCSILK_BUILD_SHARED=ON

# TSAN (mutually exclusive with ASAN)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_TSAN=ON -DENABLE_OOM_TEST=ON

# Coverage (requires gcc, no TEST_OOM)
cmake -B build -S . -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_COVERAGE=ON -DENABLE_OOM_TEST=OFF
```

## Lint & Format

```bash
cmake --build build --target check-format   # dry-run, fails on mismatch
cmake --build build --target format         # apply clang-format
cmake --build build --target tidy           # clang-tidy (WarningsAsErrors)
./scripts/check_version_sync.sh             # CMake version ↔ git tag
```

Format targets: `src/*.c`, `src/*.h`, `include/*.h`, `tests/*`, `examples/*`.

## Key Gotchas

### TEST_OOM — deterministic builds
`-DENABLE_OOM_TEST=ON` defines `TEST_OOM`, which forces deterministic salts and memory-allocation faking. Without it, bcrypt and other tests use `/dev/urandom` salts — **tests that assert hash equality must be wrapped in `#ifdef TEST_OOM`**.

### bcrypt constants (fixed Aug 2026)
- `CSILK_BCRYPT_CIPHER_OUT = 24` (not 23) — produces 32 base64 checksum chars
- `CSILK_BCRYPT_HASH_LEN = 62` — `$2a$XX$` (7) + 22 salt + 32 checksum + NUL
- `csilk_bcrypt_verify` reads 32 checksum chars from `hash + 29`

### Stack buffers in crypto
Always `memset` stack buffers before partial `memcpy`:
```c
uint8_t pwd_buf[72];
memset(pwd_buf, 0, sizeof(pwd_buf));  // required when len may be 0
memcpy(pwd_buf, password, len);
```

### Eksblowfish datal/datar initialization
`datal` and `datar` must be zeroed **before** the P-array keying loop (Step 2). Omitting this causes non-deterministic results for empty passwords.

### clang-tidy false positive
`clang-analyzer-core.uninitialized.Assign` fires on `XL ^= p[i]` in `blowfish_encipher` because the analyzer cannot trace through the array-pointer parameter. Suppressed in `.clang-tidy` — do not add `[[maybe_unused]]` or other workarounds; the code is correct.

## Source Layout

| Directory | Purpose |
|---|---|
| `src/core/` | HTTP server, arena, config, JSON, TLS |
| `src/crypto/` | base64, sha1, url, uuid, crypto primitives, bcrypt |
| `src/drivers/` | DB (sqlite, postgres, mysql, redis), AI, cipher, vector |
| `src/middleware/` | auth, cors, csrf, jwt, ratelimit, etc. |
| `src/protocols/` | websocket, h2, h3, swagger, mcp |
| `src/messaging/` | MQ core, pubsub, raft WAL/RPC/consensus |
| `src/workflow/` | agent engine, scheduler, DSL |
| `tests/` | Mirrors `src/` module layout |
| `python/` | CFFI/ctypes bindings |

## Commit Convention

```
type(scope): 🎯 subject
```

Types: `feat ✨`, `fix 🐛`, `docs 📝`, `style 🎨`, `refactor ♻️`, `test ✅`, `build 📦`, `ci 👷`, `chore 🧹`. Emoji goes **after** the colon, one per commit, subject lowercase, imperative mood.

## Python Bindings

```bash
# Build shared lib first, then run Python tests
cmake --build build --target csilk_shared
python3 python/tests/test_csilk.py
```
