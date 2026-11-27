#!/usr/bin/env bash
#
# check_uring_equivalence.sh — Verify behavioral parity between libuv and io_uring backends.
#
# Usage:
#   ./scripts/check_uring_equivalence.sh --uv-bin build/test_io_equivalence --uring-bin build_uring/test_io_equivalence
#
set -euo pipefail

UV_BIN=""
URING_BIN=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --uv-bin)    UV_BIN="$2"; shift 2 ;;
        --uring-bin) URING_BIN="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [ -z "$UV_BIN" ] || [ -z "$URING_BIN" ]; then
    echo "Usage: $0 --uv-bin <path> --uring-bin <path>"
    exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

"$UV_BIN" --emit-json "$tmpdir/uv.json"
"$URING_BIN" --emit-json "$tmpdir/uring.json"

python3 - "$tmpdir/uv.json" "$tmpdir/uring.json" << 'PYEOF'
import json, sys

with open(sys.argv[1], encoding="utf-8") as f:
    uv = json.load(f)
with open(sys.argv[2], encoding="utf-8") as f:
    ur = json.load(f)

assert uv.get("backend") == "libuv", f"unexpected uv backend: {uv.get('backend')}"
assert ur.get("backend") == "io_uring", f"unexpected uring backend: {ur.get('backend')}"

uv_cmp = {k: v for k, v in uv.items() if k != "backend"}
ur_cmp = {k: v for k, v in ur.items() if k != "backend"}

if uv_cmp != ur_cmp:
    print("FAIL: Backend behavioral mismatch detected!")
    for k in sorted(set(uv_cmp) | set(ur_cmp)):
        if uv_cmp.get(k) != ur_cmp.get(k):
            print(f"  {k}: libuv={uv_cmp.get(k)!r} != io_uring={ur_cmp.get(k)!r}")
    sys.exit(1)

print("PASS: 100% behavioral equivalence verified across libuv and io_uring backends:")
for k, v in sorted(uv_cmp.items()):
    print(f"  {k}: {v!r}")
PYEOF
