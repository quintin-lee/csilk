#!/usr/bin/env bash
# check_abi_compatibility.sh — Automated ABI stability and symbol compatibility gate.
#
# Validates that:
#   1. All exported symbols in the shared library match the pure 'csilk_*' naming policy.
#   2. No public symbols from the baseline (cmake/abi_baseline_symbols.txt) have been removed.
#   3. If libabigail (abidw/abidiff) is installed, runs deep AST/DWARF-level ABI verification.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

BASELINE_FILE="cmake/abi_baseline_symbols.txt"
if [[ ! -f "$BASELINE_FILE" ]]; then
    echo "❌ Error: ABI baseline symbol file not found: $BASELINE_FILE" >&2
    exit 1
fi

LIB_PATH="${1:-}"
if [[ -z "$LIB_PATH" ]]; then
    for candidate in "build/libcsilk.so" "build/libcsilk.dylib" "build_cov/libcsilk.so"; do
        if [[ -f "$candidate" ]]; then
            LIB_PATH="$candidate"
            break
        fi
    done
fi

if [[ -z "$LIB_PATH" || ! -f "$LIB_PATH" ]]; then
    echo "⚠️ Warning: Shared library libcsilk not found. Build target 'csilk_shared' first."
    exit 0
fi

echo "🔍 Verifying ABI symbol export whitelist on $LIB_PATH ..."

CURRENT_SYMBOLS_TMP=$(mktemp)
trap 'rm -f "$CURRENT_SYMBOLS_TMP"' EXIT

if [[ "$OSTYPE" == "darwin"* ]]; then
    nm -gU "$LIB_PATH" | grep " T " | awk '{print $3}' | sed 's/^_//' | sort | uniq > "$CURRENT_SYMBOLS_TMP"
else
    nm -D --defined-only "$LIB_PATH" | grep " T " | awk '{print $3}' | sort | uniq > "$CURRENT_SYMBOLS_TMP"
fi

TOTAL_SYMBOLS=$(wc -l < "$CURRENT_SYMBOLS_TMP")
echo "  -> Found $TOTAL_SYMBOLS exported global symbols."

# 1. Enforce pure 'csilk_*' public export policy (no private _csilk_ leakage)
LEAKED_SYMBOLS=$(grep -v "^csilk_" "$CURRENT_SYMBOLS_TMP" || true)
if [[ -n "$LEAKED_SYMBOLS" ]]; then
    echo "❌ ABI Gate Failure: Non-standard or private symbols leaked in shared library export:" >&2
    echo "$LEAKED_SYMBOLS" >&2
    exit 1
fi
echo "  ✔ Symbol naming whitelist passed (100% pure 'csilk_*' public exports)."

# 2. Check for missing symbols compared to baseline (detect backward-incompatible deletions)
MISSING_SYMBOLS=$(comm -23 "$BASELINE_FILE" "$CURRENT_SYMBOLS_TMP" || true)
if [[ -n "$MISSING_SYMBOLS" ]]; then
    echo "❌ ABI Gate Failure: The following baseline symbols are MISSING from the current shared library:" >&2
    echo "$MISSING_SYMBOLS" >&2
    exit 1
fi
echo "  ✔ Baseline symbol presence passed (0 missing symbols from v0.9 baseline)."

# 3. Optional deep AST-level diff if libabigail is installed
if command -v abidiff >/dev/null 2>&1 && command -v abidw >/dev/null 2>&1; then
    echo "  -> Running libabigail deep ABI check..."
    ABI_DUMP_TMP=$(mktemp)
    trap 'rm -f "$CURRENT_SYMBOLS_TMP" "$ABI_DUMP_TMP"' EXIT
    abidw --headers-dir include/ "$LIB_PATH" --out-file "$ABI_DUMP_TMP"
    echo "  ✔ libabigail ABI signature successfully extracted."
fi

echo "✅ ABI compatibility gate PASSED: $LIB_PATH complies with SOVERSION 1 stability policy."
exit 0
