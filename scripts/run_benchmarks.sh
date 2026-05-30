#!/usr/bin/env bash
#
# run_benchmarks.sh — Unified csilk performance benchmark suite.
#
# Requires: wrk, python3 (for JSON formatting and comparison)
#
# Usage:
#   ./scripts/run_benchmarks.sh                # run benchmarks, print summary
#   ./scripts/run_benchmarks.sh --save         # save results as benchmark JSON
#   ./scripts/run_benchmarks.sh --compare      # compare with last saved result
#   ./scripts/run_benchmarks.sh --ci           # CI mode: fail on >10% regression
#
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")/.." && pwd)/benchmarks"
RESULTS_DIR="${BENCH_DIR}/results"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build_bench"
PORT=8080
BASE_URL="http://127.0.0.1:${PORT}"

# Duration per test (default 5s, can be set via env)
DURATION="${BENCH_DURATION:-5s}"
CORES=$(nproc 2>/dev/null || echo 2)

MODE="run"
for arg in "$@"; do
    case "$arg" in
        --save)    MODE="save" ;;
        --compare) MODE="compare" ;;
        --ci)      MODE="ci" ;;
    esac
done

if ! command -v wrk &>/dev/null; then
    echo "Error: wrk not found. Install with: sudo apt-get install wrk"
    exit 1
fi

mkdir -p "$RESULTS_DIR"

# Build
echo "=== Building csilk (release) ==="
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Release 2>/dev/null
cmake --build "$BUILD_DIR" --target example_server -j"$CORES" 2>/dev/null

# Create static test files
mkdir -p "$BUILD_DIR/public"
echo '<html><body>Csilk Benchmark</body></html>' > "$BUILD_DIR/public/small.html"
dd if=/dev/urandom bs=1024 count=1 of="$BUILD_DIR/public/1k.bin" 2>/dev/null

# Write config
cat > "$BUILD_DIR/config.yaml" << 'YAML'
port: 8080
server:
  idle_timeout_ms: 30000
  read_timeout_ms: 30000
  write_timeout_ms: 30000
  max_body_size: 1048576
  max_header_size: 65536
  worker_threads: 4
static_files:
  enable: 1
  root_dir: "./public"
  prefix: "/static"
logger:
  level: ERROR
middleware:
  enable_recovery: 1
YAML

rm -f "$BUILD_DIR/server.log"

echo "=== Starting example_server on port ${PORT} ==="
cd "$BUILD_DIR"
./example_server > /dev/null 2>&1 &
SERVER_PID=$!
cd "$PROJECT_DIR"
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "Error: server failed to start"
    exit 1
fi

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

# Warmup
wrk -t2 -c10 -d2s "$BASE_URL/" > /dev/null 2>&1 || true

ensure_server() {
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "  [restarting server]"
        cd "$BUILD_DIR"
        ./example_server > /dev/null 2>&1 &
        SERVER_PID=$!
        cd "$PROJECT_DIR"
        sleep 2
    fi
}

run() {
    local name="$1" url="$2" t="$3" c="$4"
    ensure_server
    local output
    output=$(wrk -t"$t" -c"$c" -d"$DURATION" --latency --timeout 10s "$url" 2>/dev/null || true)
    local rps lat
    rps=$(echo "$output" | grep -oP 'Requests/sec:\s+\K[\d.]+' || echo "0")
    lat=$(echo "$output" | grep -oP 'Latency\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
    printf "  %-30s t=%-2d c=%-3d %10s req/s  (lat: %s)\n" "$name" "$t" "$c" "$rps" "$lat"
    printf '"%s_t%d_c%d": {"rps":%s,"latency":"%s"}' "$name" "$t" "$c" "$rps" "$lat"
}

VERSION=$(grep -m1 'set(CPACK_PACKAGE_VERSION' "$PROJECT_DIR/CMakeLists.txt" | sed 's/.*"\(.*\)".*/\1/')
GIT_HASH=$(cd "$PROJECT_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
TIMESTAMP=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

echo ""
echo "=========================================="
echo "Csilk Benchmark Suite"
echo "Version: ${VERSION} (${GIT_HASH})"
echo "Duration: ${DURATION} per test"
echo "=========================================="
echo ""

# Build JSON incrementally
tmpfile=$(mktemp)
echo "{" > "$tmpfile"
sep=""

bench() {
    local name="$1" url="$2" t="$3" c="$4"
    ensure_server
    local output rps lat
    output=$(wrk -t"$t" -c"$c" -d"$DURATION" --latency --timeout 10s "$url" 2>/dev/null || true)
    rps=$(echo "$output" | grep -oP 'Requests/sec:\s+\K[\d.]+' || echo "0")
    lat=$(echo "$output" | grep -oP 'Latency\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
    printf "  %-30s t=%-2d c=%-3d %10s req/s  (lat: %s)\n" "$name" "$t" "$c" "$rps" "$lat"
    printf '%s"%s_t%d_c%d":{"rps":%s,"latency":"%s"}' "$sep" "$name" "$t" "$c" "$rps" "$lat" >> "$tmpfile"
    sep=","
}

echo "--- Static File (zero-copy sendfile) ---"
bench "static_small" "$BASE_URL/static/small.html" 2 10
bench "static_small" "$BASE_URL/static/small.html" 4 50
bench "static_small" "$BASE_URL/static/small.html" 4 100
bench "static_1k"    "$BASE_URL/static/1k.bin"      4 100
echo ""

echo "--- JSON Response (cJSON) ---"
bench "json"         "$BASE_URL/api/data"             2 10
bench "json"         "$BASE_URL/api/data"             4 50
bench "json"         "$BASE_URL/api/data"             4 100
echo ""

echo "--- Plain Text ---"
bench "hello"        "$BASE_URL/"                     2 10
bench "hello"        "$BASE_URL/"                     4 50
bench "hello"        "$BASE_URL/"                     4 100
echo ""

echo "--- Radix Tree Routing (param match) ---"
bench "routing"      "$BASE_URL/user/42"              4 100
echo ""

echo "--- HTTP Pipelining ---"
ensure_server
pipe_output=$(wrk -t4 -c100 -d"$DURATION" --latency --timeout 10s \
    -s <(echo 'wrk.pipeline = 16') "$BASE_URL/" 2>/dev/null || true)
prps=$(echo "$pipe_output" | grep -oP 'Requests/sec:\s+\K[\d.]+' || echo "0")
plat=$(echo "$pipe_output" | grep -oP 'Latency\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
printf "  %-30s t=%-2d c=%-3d %10s req/s  (lat: %s)\n" "pipeline" 4 100 "$prps" "$plat"
printf '%s"pipeline_t4_c100":{"rps":%s,"latency":"%s"}' "$sep" "$prps" "$plat" >> "$tmpfile"
sep=","
echo ""

echo "--- OpenAPI Spec (large JSON) ---"
bench "openapi"      "$BASE_URL/openapi.json"         4 100
echo ""

# Meta
printf ',%s"meta":{"version":"%s","git_hash":"%s","timestamp":"%s","duration":"%s"}' \
    "$sep" "$VERSION" "$GIT_HASH" "$TIMESTAMP" "$DURATION" >> "$tmpfile"
echo "}" >> "$tmpfile"

echo "=========================================="
echo "Results (JSON)"
echo "=========================================="
python3 -m json.tool "$tmpfile" 2>/dev/null || cat "$tmpfile"

# --save
if [ "$MODE" = "save" ] || [ "$MODE" = "ci" ]; then
    outfile="${BENCH_DIR}/results/v${VERSION}-${GIT_HASH}.json"
    cp "$tmpfile" "$outfile"
    echo "Saved: ${outfile}"
fi

# --compare or --ci
if [ "$MODE" = "compare" ] || [ "$MODE" = "ci" ]; then
    last=$(ls -t "${BENCH_DIR}/results"/*.json 2>/dev/null | head -2 | tail -1 || true)
    if [ -z "$last" ]; then
        echo "Note: no previous result to compare against"
    else
        echo ""
        echo "=== Comparison with $(basename "$last") ==="
        regression=0
        python3 -c "
import json, sys

with open('${last}') as f:
    old = json.load(f)
with open('${tmpfile}') as f:
    new = json.load(f)

for name in sorted(new.keys()):
    if name == 'meta': continue
    o = old.get(name, {})
    n = new.get(name, {})
    old_rps = float(o.get('rps', 0))
    new_rps = float(n.get('rps', 0))
    if old_rps > 0:
        change = ((new_rps - old_rps) / old_rps) * 100
        flag = ' **REGRESSION**' if change < -10 else ''
        print(f'  {name:35s} {old_rps:8.0f} → {new_rps:8.0f} req/s  ({change:+5.1f}%){flag}')
        if change < -10:
            global reg
            reg = 1
    else:
        print(f'  {name:35s}                 {new_rps:8.0f} req/s  (no baseline)')
if reg == 1:
    sys.exit(1)
"
        rc=$?
        if [ $rc -ne 0 ] && [ "$MODE" = "ci" ]; then
            echo ""
            echo "ERROR: Performance regression detected (>10% drop in RPS)."
            echo "CI check failed."
            rm -f "$tmpfile"
            exit 1
        fi
    fi
fi

rm -f "$tmpfile"
echo ""
echo "=== Done ==="
