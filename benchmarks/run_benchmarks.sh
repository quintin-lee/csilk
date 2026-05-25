#!/bin/bash
# Csilk Performance Benchmark Suite
set -uo pipefail

PORT=8080
BASE_URL="http://localhost:$PORT"
RESULTS_DIR="benchmarks/results"
DURATION="${1:-10s}"
WARMUP=5s

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$RESULTS_DIR"

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

cd "$PROJECT_DIR"
mkdir -p build_release
cd build_release

cmake .. -DCMAKE_BUILD_TYPE=Release -DCSILK_BUILD_SHARED=OFF 2>&1 | tail -1
make -j$(nproc) example_server 2>&1 | tail -1

mkdir -p public
echo '<html><body>Csilk Benchmark</body></html>' > public/small.html
dd if=/dev/urandom bs=1024 count=1 of=public/1k.bin 2>/dev/null

cat > config.yaml << 'YAML'
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

echo "Starting server..."
./example_server > /dev/null 2>&1 &
SERVER_PID=$!
cd "$PROJECT_DIR"

sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "ERROR: Failed to start server"
  exit 1
fi

wrk -t2 -c10 -d"$WARMUP" "$BASE_URL/" > /dev/null 2>&1 || true

ensure_server() {
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[RESTARTING]"
    cd "$PROJECT_DIR"
    rm -f build_release/server.log
    cd build_release
    ./example_server > /dev/null 2>&1 &
    SERVER_PID=$!
    cd "$PROJECT_DIR"
    sleep 2
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "ERROR: Failed to restart server"
      return 1
    fi
  fi
}

run_bench() {
  local name="$1" url="$2" t="$3" c="$4" d="$5"
  local file="$RESULTS_DIR/${name}_t${t}_c${c}.txt"
  printf "  %-25s t=%-2d c=%-3d " "$name" "$t" "$c"
  ensure_server || return
  wrk -t"$t" -c"$c" -d"$d" --latency --timeout 10s "$url" 2>&1 | tee "$file" | \
    awk '/Requests\/sec:/{r=$2} END{printf "%12s req/s\n", r}'
}

run_pipeline() {
  local name="$1" url="$2" t="$3" c="$4" d="$5"
  local file="$RESULTS_DIR/${name}_t${t}_c${c}_pipeline.txt"
  printf "  %-25s t=%-2d c=%-3d pipeline " "$name" "$t" "$c"
  ensure_server || return
  wrk -t"$t" -c"$c" -d"$d" --latency --timeout 10s \
    -s <(echo 'wrk.pipeline = 16') "$url" 2>&1 | tee "$file" | \
    awk '/Requests\/sec:/{r=$2} END{printf "%12s req/s\n", r}'
}

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
HW=$(lscpu | grep "Model name" | head -1 | sed 's/Model name:\s*//')

echo ""
echo "=========================================="
echo "Csilk Benchmark Suite"
echo "Date: $TIMESTAMP"
echo "CPU: $HW"
echo "Server: $BASE_URL ($DURATION per test)"
echo "=========================================="
echo ""

echo "--- Static File (zero-copy sendfile) ---"
run_bench "static_small"  "$BASE_URL/static/small.html"  2 10 $DURATION
run_bench "static_small"  "$BASE_URL/static/small.html"  4 50 $DURATION
run_bench "static_small"  "$BASE_URL/static/small.html"  4 100 $DURATION
run_bench "static_1k"     "$BASE_URL/static/1k.bin"      4 100 $DURATION
echo ""

echo "--- JSON Response (cJSON) ---"
run_bench "json"          "$BASE_URL/api/data"           2 10 $DURATION
run_bench "json"          "$BASE_URL/api/data"           4 50 $DURATION
run_bench "json"          "$BASE_URL/api/data"           4 100 $DURATION
echo ""

echo "--- Plain Text ---"
run_bench "hello"         "$BASE_URL/"                  2 10 $DURATION
run_bench "hello"         "$BASE_URL/"                  4 50 $DURATION
run_bench "hello"         "$BASE_URL/"                  4 100 $DURATION
echo ""

echo "--- Radix Tree Routing (param match) ---"
run_bench "routing"       "$BASE_URL/user/42"            4 100 $DURATION
echo ""

echo "--- HTTP Pipelining ---"
run_pipeline "hello"      "$BASE_URL/"                  4 100 $DURATION
echo ""

echo "--- OpenAPI Spec (large JSON) ---"
run_bench "openapi"       "$BASE_URL/openapi.json"       4 100 $DURATION
echo ""

echo "=========================================="
echo "SUMMARY"
echo "=========================================="
for f in "$RESULTS_DIR"/*.txt; do
  name=$(basename "$f" .txt)
  rps=$(grep "Requests/sec:" "$f" | awk '{print $2}')
  lat=$(grep "^    Latency" "$f" | awk '{print $2}')
  printf "  %-45s %10s req/s  (lat: %s)\n" "$name" "${rps:-N/A}" "${lat:-N/A}"
done | sort
echo ""
echo "Results: $RESULTS_DIR/"
