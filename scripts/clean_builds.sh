#!/usr/bin/env bash
#
# clean_builds.sh — List CMake build trees and remove all but the canonical ones.
#
# The repo accumulates one-off "-B build_<variant>" trees from sanitizer,
# backend, and benchmark experiments (each can be 0.5-1.5 GB). This script
# keeps only the canonical trees used by day-to-day development and the CI
# simulations, and prunes the rest.
#
# Usage:
#   ./scripts/clean_builds.sh              # dry-run: list keep / prune + sizes
#   ./scripts/clean_builds.sh --prune      # actually delete the prunable trees
#   ./scripts/clean_builds.sh --keep build,build_uring,build_asan --prune
#   ./scripts/clean_builds.sh -h|--help
#
# Exit codes: 0 on success (dry-run included), 1 on bad usage.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Canonical trees kept by default:
#   build        - libuv (default backend, matches the main CI matrix)
#   build_uring  - native io_uring (matches the CI uring compatibility job)
#   build_asan   - ASAN libuv (matches the CI ASAN runs)
#   build_tsan   - TSAN (matches the CI TSAN job)
#   build_fuzz   - libFuzzer + ASAN (matches the CI fuzz job)
KEEP_DEFAULT="build,build_uring,build_asan,build_tsan,build_fuzz"

PRUNE=false
KEEP="$KEEP_DEFAULT"

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prune|-p)
      PRUNE=true
      shift
      ;;
    --keep|-k)
      KEEP="${2:-$KEEP_DEFAULT}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option: $1 (see --help)" >&2
      exit 1
      ;;
  esac
done

cd "$PROJECT_DIR"

# Every build tree follows the documented "build_<variant>" / "build" /
# "build-lint" naming and is gitignored; anything else in the workspace is
# not ours to touch.
shopt -s nullglob
trees=(build build-*)
trees+=(build_*)
shopt -u nullglob

if [[ ${#trees[@]} -eq 0 ]]; then
  echo "No build trees found in $PROJECT_DIR."
  exit 0
fi

# A directory only counts as a CMake build tree if it carries a CMake cache;
# guards against pruning unrelated "build*" directories a user may have.
is_build_tree() {
  [[ -f "$1/CMakeCache.txt" ]]
}

declare -A keep_map=()
IFS=',' read -ra keep_list <<< "$KEEP"
for k in "${keep_list[@]}"; do
  keep_map["$k"]=1
done

total_freed=0
pruned_any=false

# Human-readable reclaim size (MB below 1 GB, GB above).
fmt_size() {
  awk -v kb="$1" 'BEGIN {
    if (kb >= 1048576) printf "%.1f GB", kb / 1048576;
    else printf "%d MB", kb / 1024;
  }'
}

printf '%-30s %10s  %s\n' "TREE" "SIZE" "ACTION"
printf '%-30s %10s  %s\n' "----" "----" "------"

for t in "${trees[@]}"; do
  is_build_tree "$t" || continue
  size_kb=$(du -sk "$t" 2>/dev/null | cut -f1)
  size_h=$(du -sh "$t" 2>/dev/null | cut -f1)
  if [[ -n "${keep_map[$t]:-}" ]]; then
    printf '%-30s %10s  %s\n' "$t" "$size_h" "KEEP"
  else
    printf '%-30s %10s  %s\n' "$t" "$size_h" "PRUNE"
    if $PRUNE; then
      rm -rf "$t"
      pruned_any=true
    fi
    total_freed=$((total_freed + size_kb))
  fi
done

if ! $PRUNE; then
  echo
  echo "Dry run: nothing deleted."
  echo "Candidates reclaim ~$(fmt_size "$total_freed")."
  echo "Run '$0 --prune' to delete them, or pass --keep a,b,c to change the keep list."
elif $pruned_any; then
  echo
  echo "Pruned. Reclaimed ~$(fmt_size "$total_freed")."
else
  echo
  echo "Nothing to prune — only canonical trees present."
fi
