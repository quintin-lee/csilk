#!/usr/bin/env bash
#
# run_benchmarks.sh — Automated performance benchmark suite for csilk.
#
# Requires: wrk, jq (optional for JSON formatting)
# Builds the example_server in release mode, starts it, runs wrk
# against several endpoints, and saves results as JSON.
#
# Usage:
#   ./scripts/run_benchmarks.sh              # run benchmarks
#   ./scripts/run_benchmarks.sh --save       # save to benchmarks/
#   ./scripts/run_benchmarks.sh --compare    # compare with last saved result
#
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")/.." && pwd)/benchmarks"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build_bench"
PORT=8080
BASE_URL="http://127.0.0.1:${PORT}"
DURATION=5
CONNECTIONS=100
THREADS=4

SAVE=false
COMPARE=false
for arg in "$@"; do
    case "$arg" in
        --save) SAVE=true ;;
        --compare) COMPARE=true ;;
    esac
done

if ! command -v wrk &>/dev/null; then
    echo "Error: wrk not found. Install with: sudo apt-get install wrk"
    exit 1
fi

echo "=== Building csilk (release) ==="
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Release 2>/dev/null
cmake --build "$BUILD_DIR" --target example_server -j"$(nproc)" 2>/dev/null

VERSION=$(grep -m1 'set(CPACK_PACKAGE_VERSION' "$PROJECT_DIR/CMakeLists.txt" | sed 's/.*"\(.*\)".*/\1/')
GIT_HASH=$(cd "$PROJECT_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")

echo "=== Starting example_server on port ${PORT} ==="
cd "$BUILD_DIR"
./example_server &
SERVER_PID=$!
cd "$PROJECT_DIR"
sleep 1

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "Error: server failed to start"
    exit 1
fi

cleanup() {
    echo "=== Stopping server ==="
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

echo ""
echo "=== Running Benchmarks (${DURATION}s each, ${CONNECTIONS} connections, ${THREADS} threads) ==="
echo ""

declare -A SCENARIOS
SCENARIOS["hello"]="${BASE_URL}/"
SCENARIOS["api_data"]="${BASE_URL}/api/data"
SCENARIOS["large"]="${BASE_URL}/api/large"

results_file=$(mktemp)
echo "{" > "$results_file"
first=true

for name in "${!SCENARIOS[@]}"; do
    url="${SCENARIOS[$name]}"
    echo "--- Benchmark: ${name} (${url}) ---"

    output=$(wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" --latency "$url" 2>/dev/null || true)

    req_sec=$(echo "$output" | grep -oP 'Requests/sec:\s+\K[\d.]+' || echo "0")
    latency_avg=$(echo "$output" | grep -oP 'Latency\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
    p50=$(echo "$output" | grep -oP '50%\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
    p99=$(echo "$output" | grep -oP '99%\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")

    echo "  Requests/sec: ${req_sec}"
    echo "  Latency: avg=${latency_avg}  p50=${p50}  p99=${p99}"
    echo ""

    $first || echo "," >> "$results_file"
    first=false
    cat >> "$results_file" <<JSON_DATA
    "${name}": {
      "url": "${url}",
      "requests_per_sec": ${req_sec},
      "latency_avg": "${latency_avg}",
      "latency_p50": "${p50}",
      "latency_p99": "${p99}"
    }
JSON_DATA
done

echo "," >> "$results_file"
cat >> "$results_file" <<JSON_DATA
    "meta": {
      "version": "${VERSION}",
      "git_hash": "${GIT_HASH}",
      "duration_seconds": ${DURATION},
      "connections": ${CONNECTIONS},
      "threads": ${THREADS}
    }
JSON_DATA
echo "}" >> "$results_file"

echo "=== Results ==="
python3 -m json.tool "$results_file" 2>/dev/null || cat "$results_file"

if $SAVE; then
    filename="${BENCH_DIR}/v${VERSION}-${GIT_HASH}.json"
    cp "$results_file" "$filename"
    echo "Saved to: ${filename}"
fi

if $COMPARE; then
    last_file=$(ls -t "${BENCH_DIR}"/*.json 2>/dev/null | head -1)
    if [ -n "$last_file" ]; then
        echo ""
        echo "=== Comparison with $(basename "$last_file") ==="
        python3 -c "
import json, sys
with open('${last_file}') as f:
    old = json.load(f)
with open('${results_file}') as f:
    new = json.load(f)
for name in sorted(new.keys()):
    if name == 'meta': continue
    o = old.get(name, {})
    n = new.get(name, {})
    old_rps = float(o.get('requests_per_sec', 0))
    new_rps = float(n.get('requests_per_sec', 0))
    if old_rps > 0:
        change = ((new_rps - old_rps) / old_rps) * 100
        print(f'  {name}: {old_rps:.0f} -> {new_rps:.0f} req/s ({change:+.1f}%)')
    else:
        print(f'  {name}: {new_rps:.0f} req/s (no previous data)')
"
    fi
fi

rm -f "$results_file"
echo ""
echo "=== Done ==="
