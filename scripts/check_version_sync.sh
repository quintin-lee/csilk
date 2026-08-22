#!/usr/bin/env bash
#
# check_version_sync.sh — Verify CMake version matches git tag or expected release version.
#
# Usage:
#   ./scripts/check_version_sync.sh                      # Verify CMake version == latest git tag
#   ./scripts/check_version_sync.sh --expected 0.5.1     # Verify CMake version == 0.5.1
#   ./scripts/check_version_sync.sh --ci                 # CI mode
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

EXPECTED_VERSION=""
CI_MODE=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --expected|--target|-e|-t)
      EXPECTED_VERSION="${2:-}"
      shift 2
      ;;
    --ci)
      CI_MODE=true
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--expected <version>] [--ci]"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

# Extract version from CMakeLists.txt / cmake/options.cmake
CMAKE_VERSION=$(grep -E '^set\(CSILK_VERSION_(MAJOR|MINOR|PATCH)' "$PROJECT_DIR/CMakeLists.txt" "$PROJECT_DIR/cmake/options.cmake" 2>/dev/null \
  | grep -oE '[0-9]+' \
  | tr '\n' '.' \
  | sed 's/\.$//')

if [ -z "$CMAKE_VERSION" ]; then
  echo "ERROR: Could not extract version from CMakeLists.txt or cmake/options.cmake"
  exit 1
fi

echo "CMake version: $CMAKE_VERSION"

# If expected version was explicitly provided, verify CMake matches it
if [ -n "$EXPECTED_VERSION" ]; then
  EXPECTED_CLEAN="${EXPECTED_VERSION#v}"
  echo "Expected version: $EXPECTED_CLEAN"
  if [ "$CMAKE_VERSION" = "$EXPECTED_CLEAN" ]; then
    echo "✓ Version sync OK: $CMAKE_VERSION matches expected $EXPECTED_CLEAN"
    exit 0
  else
    echo "✗ Version mismatch: CMake=$CMAKE_VERSION, expected=$EXPECTED_CLEAN"
    exit 1
  fi
fi

# Otherwise, verify CMake matches latest git tag
GIT_TAG=$(git -C "$PROJECT_DIR" describe --tags --abbrev=0 2>/dev/null || echo "")
GIT_TAG_CLEAN="${GIT_TAG#v}"

echo "Latest git tag: ${GIT_TAG:-<none>}"

if [ -z "$GIT_TAG_CLEAN" ]; then
  echo "WARNING: No git tags found. Cannot verify version sync against git tag."
  exit 0
fi

if [ "$CMAKE_VERSION" = "$GIT_TAG_CLEAN" ]; then
  echo "✓ Version sync OK: $CMAKE_VERSION"
  exit 0
else
  echo "✗ Version mismatch: CMakeLists.txt=$CMAKE_VERSION, git tag=$GIT_TAG_CLEAN"
  echo ""
  echo "To fix:"
  echo "  1. Update CSILK_VERSION_MAJOR/MINOR/PATCH in cmake/options.cmake to match the release tag"
  echo "  2. Or create a new git tag:  git tag v$CMAKE_VERSION"
  echo "  3. Or run: ./scripts/tag-release.sh"
  exit 1
fi
