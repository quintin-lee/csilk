#!/usr/bin/env bash
#
# Package precompiled csilk artifacts for distribution.
# Usage: ./scripts/package.sh [--shared]
#
# Builds in Release mode, installs to a staging directory,
# and creates a tarball suitable for GitHub Releases.
set -euo pipefail
set -x

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-pkg"
STAGING_DIR="${ROOT_DIR}/build-pkg/staging"
CSILK_BUILD_SHARED=OFF

while [[ $# -gt 0 ]]; do
    case "$1" in
        --shared) CSILK_BUILD_SHARED=ON ;;
        *) echo "Usage: $0 [--shared]"; exit 1 ;;
    esac
    shift
done

# 1. Configure & build (Release)
echo "=== Configuring (Release) ==="
EXTRA_FLAGS=""
if [ -n "${CSILK_EXTRA_CMAKE_FLAGS:-}" ]; then
  EXTRA_FLAGS="${CSILK_EXTRA_CMAKE_FLAGS}"
fi
cmake -B "$BUILD_DIR" -S "$ROOT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$STAGING_DIR" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCSILK_BUILD_SHARED="$CSILK_BUILD_SHARED" \
    -DENABLE_OOM_TEST=OFF \
    -DCJSON_INSTALL=OFF \
    ${EXTRA_FLAGS}

echo "=== Building ==="
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

# Extract version from CMake cache
CSILK_VERSION=$(grep '^CSILK_VERSION:' "$BUILD_DIR"/CMakeCache.txt | cut -d= -f2-)
if [ -z "$CSILK_VERSION" ]; then
  CSILK_VERSION=$(grep '^CSILK_VERSION_MAJOR:' "$BUILD_DIR"/CMakeCache.txt | cut -d= -f2-)
  CSILK_VERSION="${CSILK_VERSION}.$(grep '^CSILK_VERSION_MINOR:' "$BUILD_DIR"/CMakeCache.txt | cut -d= -f2-)"
  CSILK_VERSION="${CSILK_VERSION}.$(grep '^CSILK_VERSION_PATCH:' "$BUILD_DIR"/CMakeCache.txt | cut -d= -f2-)"
fi
if [ -z "$CSILK_VERSION" ]; then
  echo "ERROR: Could not determine CSILK_VERSION from CMake cache" >&2
  exit 1
fi
echo "=== Version: ${CSILK_VERSION} ==="

# 2. Install to staging
echo "=== Installing to staging ==="
cmake --install "$BUILD_DIR" --prefix "$STAGING_DIR" --verbose

if [ "$CSILK_BUILD_SHARED" = "ON" ]; then
    echo "=== Installing csilk_shared component ==="
    cmake --install "$BUILD_DIR" --prefix "$STAGING_DIR" --component csilk_shared --verbose 2>/dev/null || echo "(csilk_shared component not found, skipping)"
fi

# 3. Determine platform label
OS_LABEL=""
ARCH=""
case "$(uname -s)" in
    Linux)  OS_LABEL="linux" ;;
    Darwin) OS_LABEL="macos"  ;;
    *)      OS_LABEL="unknown" ;;
esac
case "$(uname -m)" in
    x86_64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    *)        ARCH="$(uname -m)" ;;
esac

PKG_NAME="csilk-${CSILK_VERSION}-${OS_LABEL}-${ARCH}"
PKG_DIR="${BUILD_DIR}/${PKG_NAME}"

# 4. Rename staging to package name
mv "$STAGING_DIR" "$PKG_DIR"

# 5. Strip debug symbols from libraries (Release already strips, but enforce)
echo "=== Stripping debug symbols ==="
if [ -f "$PKG_DIR/lib/libcsilk.a" ]; then
    strip --strip-debug "$PKG_DIR/lib/libcsilk.a" 2>/dev/null || echo "(strip failed for libcsilk.a)"
fi
if [ -f "$PKG_DIR/lib/libcsilk.so" ]; then
    strip --strip-unneeded "$PKG_DIR/lib/libcsilk.so" 2>/dev/null || echo "(strip failed for libcsilk.so)"
fi

# 6. Create tarball
echo "=== Creating tarball: ${PKG_NAME}.tar.gz ==="
tar -C "$BUILD_DIR" -czvf "${BUILD_DIR}/${PKG_NAME}.tar.gz" "$PKG_NAME"

# 7. Also create checksum
echo "=== Creating checksum ==="
cd "$BUILD_DIR"
sha256sum "${PKG_NAME}.tar.gz" > "${PKG_NAME}.tar.gz.sha256"

echo ""
echo "=== Package ready ==="
echo "  ${PKG_NAME}.tar.gz"
echo "  ${PKG_NAME}.tar.gz.sha256"
echo ""
echo "To upload to GitHub Release:"
echo "  gh release create v${CSILK_VERSION} \\"
echo "      ${PKG_NAME}.tar.gz ${PKG_NAME}.tar.gz.sha256 \\"
echo "      --title 'v${CSILK_VERSION}' --notes '...'"
