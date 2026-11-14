#!/usr/bin/env bash
#
# check_sync_primitives.sh — Enforce use of csilk/core/sync.h abstractions
# instead of raw POSIX <pthread.h> / pthread_* APIs in src/.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

violations=$(grep -rnE '(#include\s*<pthread\.h>|\bpthread_[a-zA-Z0-9_]+)' "$PROJECT_DIR/src" || true)

if [ -n "$violations" ]; then
  echo "✗ Raw pthread usage detected in src/ (use include/csilk/core/sync.h primitives instead):"
  echo "$violations"
  exit 1
fi

echo "✓ Sync primitives check passed: no raw pthread usage in src/"
