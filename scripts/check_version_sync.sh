#!/usr/bin/env bash
#
# check_version_sync.sh — Verify CMake version matches latest git tag.
#
# Usage:
#   ./scripts/check_version_sync.sh            # check only
#   ./scripts/check_version_sync.sh --ci       # CI mode: exit 1 on mismatch
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Extract version from CMakeLists.txt
CMAKE_VERSION=$(grep -E '^set\(CSILK_VERSION_(MAJOR|MINOR|PATCH)' "$PROJECT_DIR/CMakeLists.txt" \
  | grep -oE '[0-9]+' \
  | tr '\n' '.' \
  | sed 's/\.$//')

if [ -z "$CMAKE_VERSION" ]; then
  echo "ERROR: Could not extract version from CMakeLists.txt"
  exit 1
fi

# Get latest git tag
GIT_TAG=$(git -C "$PROJECT_DIR" describe --tags --abbrev=0 2>/dev/null || echo "")

# Strip leading 'v' if present
GIT_TAG_CLEAN="${GIT_TAG#v}"

echo "CMake version: $CMAKE_VERSION"
echo "Latest git tag: ${GIT_TAG:-<none>}"

if [ -z "$GIT_TAG_CLEAN" ]; then
  echo "WARNING: No git tags found. Cannot verify version sync."
  exit 0
fi

if [ "$CMAKE_VERSION" = "$GIT_TAG_CLEAN" ]; then
  echo "✓ Version sync OK: $CMAKE_VERSION"
  exit 0
else
  echo "✗ Version mismatch: CMakeLists.txt=$CMAKE_VERSION, git tag=$GIT_TAG_CLEAN"
  echo ""
  echo "To fix:"
  echo "  1. Update CSILK_VERSION_MAJOR/MINOR/PATCH in CMakeLists.txt to match the release tag"
  echo "  2. Or create a new git tag:  git tag v$CMAKE_VERSION"
  exit 1
fi
