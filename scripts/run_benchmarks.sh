#!/usr/bin/env bash
#
# run_benchmarks.sh — Unified csilk performance benchmark suite.
#
# Runs each benchmark multiple times and reports variance-normalized
# statistics (median, min, max, stddev) for reliable CI comparison.
#
# Requires: wrk, python3
#
# Usage:
#   ./scripts/run_benchmarks.sh                          # run benchmarks, print summary
#   ./scripts/run_benchmarks.sh --runs 5                 # run each benchmark 5 times
#   ./scripts/run_benchmarks.sh --save                   # save results + run stats
#   ./scripts/run_benchmarks.sh --compare                # compare with last saved result
#   ./scripts/run_benchmarks.sh --ci                     # CI mode: fail on >5% regression
#
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")/.." && pwd)/benchmarks"
RESULTS_DIR="${BENCH_DIR}/results"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build_bench"
PORT=8080
BASE_URL="http://127.0.0.1:${PORT}"

# Duration per run (default 5s, can be set via env)
DURATION="${BENCH_DURATION:-5s}"

# Number of wrk runs per benchmark (default 3)
RUNS="${BENCH_RUNS:-3}"

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

VERSION=$(grep -m1 'set(CPACK_PACKAGE_VERSION' "$PROJECT_DIR/CMakeLists.txt" | sed 's/.*"\(.*\)".*/\1/')
GIT_HASH=$(cd "$PROJECT_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
TIMESTAMP=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

echo ""
echo "=========================================="
echo "Csilk Benchmark Suite"
echo "Version: ${VERSION} (${GIT_HASH})"
echo "Duration: ${DURATION} per run"
echo "Runs:     ${RUNS} per benchmark"
echo "=========================================="
echo ""

# Build JSON incrementally
tmpfile=$(mktemp)
echo "{" > "$tmpfile"
sep=""

bench() {
    local name="$1" url="$2" t="$3" c="$4"
    local rps_values=()
    local lat_values=()

    printf "  %-30s t=%-2d c=%-3d " "$name" "$t" "$c"

    for ((i=1; i<=RUNS; i++)); do
        ensure_server
        local output rps lat
        output=$(wrk -t"$t" -c"$c" -d"$DURATION" --latency --timeout 10s "$url" 2>/dev/null || true)
        rps=$(echo "$output" | grep -oP 'Requests/sec:\s+\K[\d.]+' || echo "0")
        lat=$(echo "$output" | grep -oP 'Latency\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
        rps_values+=("$rps")
        lat_values+=("$lat")

        printf "%s req/s  " "$rps"

        if [ "$i" -lt "$RUNS" ]; then
            sleep 0.5
        fi
    done
    echo ""

    # Compute aggregated statistics using python3
    local stats_json
    stats_json=$(python3 -c "
import json, math, sys

rps = [float(x) for x in '${rps_values[*]}'.split()]
lats_raw = '${lat_values[*]}'.split()
n = len(rps)

def parse_lat(ls):
    ls = ls.strip()
    if ls.endswith('ms'):
        return float(ls[:-2])
    elif ls.endswith('us'):
        return float(ls[:-2]) / 1000.0
    elif ls.endswith('s'):
        return float(ls[:-1]) * 1000.0
    return 0.0

sorted_rps = sorted(rps)
if n % 2 == 1:
    rps_median = sorted_rps[n // 2]
else:
    rps_median = (sorted_rps[n // 2 - 1] + sorted_rps[n // 2]) / 2.0

rps_mean = sum(rps) / n
rps_min = min(rps)
rps_max = max(rps)
rps_stddev = math.sqrt(sum((x - rps_mean)**2 for x in rps) / (n - 1)) if n > 1 else 0.0
cv = (rps_stddev / rps_mean * 100) if rps_mean > 0 else 0

lats_ms = [parse_lat(l) for l in lats_raw]
sorted_lats = sorted(lats_ms)
if n % 2 == 1:
    lat_median_ms = sorted_lats[n // 2]
else:
    lat_median_ms = (sorted_lats[n // 2 - 1] + sorted_lats[n // 2]) / 2.0

def fmt_lat(ms):
    if ms >= 1.0:
        return f'{ms:.2f}ms'
    else:
        return f'{ms * 1000:.0f}us'

result = {
    'rps': round(rps_median, 1),
    'latency': fmt_lat(lat_median_ms),
    'runs': [{'rps': round(r, 1), 'latency': l} for r, l in zip(rps, lats_raw)],
    'rps_median': round(rps_median, 1),
    'rps_mean': round(rps_mean, 1),
    'rps_min': round(rps_min, 1),
    'rps_max': round(rps_max, 1),
    'rps_stddev': round(rps_stddev, 1),
    'latency_median': fmt_lat(lat_median_ms),
    'latency_min': fmt_lat(min(lats_ms)),
    'latency_max': fmt_lat(max(lats_ms)),
}
print(json.dumps(result))
" 2>/dev/null) || {
        stats_json='{"error":"stats computation failed"}'
    }

    local smin smax smed sstdv scv
    smed=$(echo "$stats_json" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)["rps_median"]))' 2>/dev/null || echo "?")
    smin=$(echo "$stats_json" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)["rps_min"]))' 2>/dev/null || echo "?")
    smax=$(echo "$stats_json" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)["rps_max"]))' 2>/dev/null || echo "?")
    sstdv=$(echo "$stats_json" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rps_stddev"])' 2>/dev/null || echo "?")
    scv=$(echo "$stats_json" | python3 -c 'import json,sys; d=json.load(sys.stdin); cv=(d["rps_stddev"]/d["rps_mean"]*100) if d["rps_mean"]>0 else 0; print(f"{cv:.1f}%")' 2>/dev/null || echo "N/A")
    echo "    median=${smed}  min=${smin}  max=${smax}  sd=${sstdv}  CV=${scv}"

    printf '%s"%s_t%d_c%d":%s' "$sep" "$name" "$t" "$c" "$stats_json" >> "$tmpfile"
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
pipe_rps_values=()
pipe_lat_values=()
printf "  %-30s t=%-2d c=%-3d " "pipeline" 4 100
for ((i=1; i<=RUNS; i++)); do
    ensure_server
    pipe_output=$(wrk -t4 -c100 -d"$DURATION" --latency --timeout 10s \
        -s <(echo 'wrk.pipeline = 16') "$BASE_URL/" 2>/dev/null || true)
    prps=$(echo "$pipe_output" | grep -oP 'Requests/sec:\s+\K[\d.]+' || echo "0")
    plat=$(echo "$pipe_output" | grep -oP 'Latency\s+\K[\d.]+(?:ms|us|s)' || echo "N/A")
    pipe_rps_values+=("$prps")
    pipe_lat_values+=("$plat")
    printf "%s req/s  " "$prps"
    [ "$i" -lt "$RUNS" ] && sleep 0.5
done
echo ""

pipe_stats=$(python3 -c "
import json, math, sys

rps = [float(x) for x in '${pipe_rps_values[*]}'.split()]
lats_raw = '${pipe_lat_values[*]}'.split()
n = len(rps)

def parse_lat(ls):
    ls = ls.strip()
    if ls.endswith('ms'):
        return float(ls[:-2])
    elif ls.endswith('us'):
        return float(ls[:-2]) / 1000.0
    elif ls.endswith('s'):
        return float(ls[:-1]) * 1000.0
    return 0.0

sorted_rps = sorted(rps)
if n % 2 == 1:
    rps_median = sorted_rps[n // 2]
else:
    rps_median = (sorted_rps[n // 2 - 1] + sorted_rps[n // 2]) / 2.0

rps_mean = sum(rps) / n
rps_min = min(rps)
rps_max = max(rps)
rps_stddev = math.sqrt(sum((x - rps_mean)**2 for x in rps) / (n - 1)) if n > 1 else 0.0

lats_ms = [parse_lat(l) for l in lats_raw]
sorted_lats = sorted(lats_ms)
if n % 2 == 1:
    lat_median_ms = sorted_lats[n // 2]
else:
    lat_median_ms = (sorted_lats[n // 2 - 1] + sorted_lats[n // 2]) / 2.0

def fmt_lat(ms):
    if ms >= 1.0:
        return f'{ms:.2f}ms'
    else:
        return f'{ms * 1000:.0f}us'

result = {
    'rps': round(rps_median, 1),
    'latency': fmt_lat(lat_median_ms),
    'runs': [{'rps': round(r, 1), 'latency': l} for r, l in zip(rps, lats_raw)],
    'rps_median': round(rps_median, 1),
    'rps_mean': round(rps_mean, 1),
    'rps_min': round(rps_min, 1),
    'rps_max': round(rps_max, 1),
    'rps_stddev': round(rps_stddev, 1),
    'latency_median': fmt_lat(lat_median_ms),
    'latency_min': fmt_lat(min(lats_ms)),
    'latency_max': fmt_lat(max(lats_ms)),
}
print(json.dumps(result))
" 2>/dev/null) || pipe_stats='{"error":"stats computation failed"}'

pmed=$(echo "$pipe_stats" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)["rps_median"]))' 2>/dev/null || echo "?")
pmin=$(echo "$pipe_stats" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)["rps_min"]))' 2>/dev/null || echo "?")
pmax=$(echo "$pipe_stats" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)["rps_max"]))' 2>/dev/null || echo "?")
pstdv=$(echo "$pipe_stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rps_stddev"])' 2>/dev/null || echo "?")
pcv=$(echo "$pipe_stats" | python3 -c 'import json,sys; d=json.load(sys.stdin); cv=(d["rps_stddev"]/d["rps_mean"]*100) if d["rps_mean"]>0 else 0; print(f"{cv:.1f}%")' 2>/dev/null || echo "N/A")
echo "    median=${pmed}  min=${pmin}  max=${pmax}  sd=${pstdv}  CV=${pcv}"

printf '%s"pipeline_t4_c100":%s' "$sep" "$pipe_stats" >> "$tmpfile"
sep=","
echo ""

echo "--- OpenAPI Spec (large JSON) ---"
bench "openapi"      "$BASE_URL/openapi.json"         4 100
echo ""

# Meta
printf '%s"meta":{"version":"%s","git_hash":"%s","timestamp":"%s","duration":"%s","runs_per_benchmark":%s}' \
    "$sep" "$VERSION" "$GIT_HASH" "$TIMESTAMP" "$DURATION" "$RUNS" >> "$tmpfile"
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
        warning=0
        python3 -c "
import json, sys

with open('${last}') as f:
    old = json.load(f)
with open('${tmpfile}') as f:
    new = json.load(f)

# Use rps_median for comparison (fall back to rps for old format)
for name in sorted(new.keys()):
    if name == 'meta': continue
    o = old.get(name, {})
    n = new.get(name, {})
    old_rps = float(o.get('rps_median', o.get('rps', 0)))
    new_rps = float(n.get('rps_median', n.get('rps', 0)))
    new_stddev = float(n.get('rps_stddev', 0))
    new_mean = float(n.get('rps_mean', new_rps))

    if old_rps > 0:
        change = ((new_rps - old_rps) / old_rps) * 100
        cv = (new_stddev / new_mean * 100) if new_mean > 0 else 0
        cv_flag = ''
        if cv > 10:
            cv_flag = '  [high variance CV={:.1f}%]'.format(cv)
            global warning
            warning = 1
        flag = '  **REGRESSION**' if change < -5 else ''
        print(f'  {name:35s} {old_rps:8.0f} → {new_rps:8.0f} req/s  ({change:+5.1f}%){flag}{cv_flag}')
        if change < -5:
            global reg
            reg = 1
    else:
        print(f'  {name:35s}                 {new_rps:8.0f} req/s  (no baseline)')

if reg == 1:
    sys.exit(2)
if warning == 1:
    sys.exit(1)
" 2>&1 || true
        rc=$?
        if [ "$rc" -eq 2 ]; then
            echo ""
            echo "ERROR: Performance regression detected (>5% drop in median RPS)."
            echo "CI check failed."
            if [ "$MODE" = "ci" ]; then
                rm -f "$tmpfile"
                exit 1
            fi
        elif [ "$rc" -eq 1 ]; then
            echo ""
            echo "WARNING: High variance detected (CV > 10%) in some benchmarks."
            echo "Results may be unreliable; consider increasing --runs."
        fi
    fi
fi

rm -f "$tmpfile"
echo ""
echo "=== Done ==="
