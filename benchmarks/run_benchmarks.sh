#!/bin/bash
# Csilk Performance Benchmark Suite
# Refactored for automated CI and regression testing
set -uo pipefail

# Configuration
PORT=8080
BASE_URL="http://localhost:$PORT"
RESULTS_DIR="benchmarks/results"
DURATION="${BENCH_DURATION:-10s}"
WARMUP=3s
SAVE_MODE=0
COMPARE_MODE=0
CI_MODE=0
THRESHOLD=10 # Regression threshold in percentage

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$RESULTS_DIR"

# Parse arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --save) SAVE_MODE=1; shift ;;
    --compare) COMPARE_MODE=1; shift ;;
    --ci) CI_MODE=1; shift ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Build server
echo "Building server in Release mode..."
cd "$PROJECT_DIR"
mkdir -p build_release
cd build_release
cmake .. -DCMAKE_BUILD_TYPE=Release -DCSILK_BUILD_SHARED=OFF > /dev/null 2>&1
make -j$(nproc) example_server > /dev/null 2>&1

# Prepare static files
mkdir -p public
echo '<html><body>Csilk Benchmark</body></html>' > public/small.html
dd if=/dev/urandom bs=1024 count=1 of=public/1k.bin 2>/dev/null

# Generate config
cat > config.yaml << 'YAML'
port: 8080
server:
  worker_threads: 4
logger:
  level: ERROR
static_files:
  enable: 1
  root_dir: "./public"
  prefix: "/static"
YAML

# Start server
echo "Starting server..."
./example_server > /dev/null 2>&1 &
SERVER_PID=$!
cd "$PROJECT_DIR"

sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "ERROR: Failed to start server"
  exit 1
fi

echo "Warmup ($WARMUP)..."
wrk -t2 -c10 -d"$WARMUP" "$BASE_URL/" > /dev/null 2>&1 || true

# Initialize JSON output
JSON_FILE="$RESULTS_DIR/bench_$(date +%s).json"
echo "{" > "$JSON_FILE"
echo "  \"timestamp\": $(date +%s)," >> "$JSON_FILE"
echo "  \"results\": [" >> "$JSON_FILE"

FIRST_RESULT=1

run_bench() {
  local name="$1" url="$2" t="$3" c="$4" d="$5" pipe="${6:-0}"
  local txt_file="$RESULTS_DIR/${name}_t${t}_c${c}.txt"
  
  printf "  %-25s t=%-2d c=%-3d " "$name" "$t" "$c"
  if [ "$pipe" -gt 0 ]; then printf "pipeline "; fi
  
  local wrk_args="-t$t -c$c -d$d --latency --timeout 10s"
  if [ "$pipe" -gt 0 ]; then
    wrk_output=$(wrk $wrk_args -s <(echo "wrk.pipeline = $pipe") "$url" 2>&1)
  else
    wrk_output=$(wrk $wrk_args "$url" 2>&1)
  fi
  
  echo "$wrk_output" > "$txt_file"
  local rps=$(echo "$wrk_output" | grep "Requests/sec:" | awk '{print $2}')
  local lat=$(echo "$wrk_output" | grep "^    Latency" | awk '{print $2}')
  
  printf "%12s req/s  (lat: %s)\n" "$rps" "$lat"
  
  if [ $FIRST_RESULT -eq 0 ]; then echo "," >> "$JSON_FILE"; fi
  echo "    {\"name\": \"$name\", \"t\": $t, \"c\": $c, \"pipeline\": $pipe, \"rps\": ${rps:-0}, \"latency\": \"$lat\"}" >> "$JSON_FILE"
  FIRST_RESULT=0
}

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo ""
echo "=========================================="
echo "Csilk Benchmark Suite ($TIMESTAMP)"
echo "Duration: $DURATION per test"
echo "=========================================="

echo "--- Static File (sendfile) ---"
run_bench "static_small"  "$BASE_URL/static/small.html"  4 100 $DURATION
run_bench "static_1k"     "$BASE_URL/static/1k.bin"      4 100 $DURATION

echo "--- JSON Response ---"
run_bench "json"          "$BASE_URL/api/data"           4 100 $DURATION

echo "--- Plain Text ---"
run_bench "hello"         "$BASE_URL/"                  4 100 $DURATION

echo "--- Radix Routing ---"
run_bench "routing"       "$BASE_URL/user/42"            4 100 $DURATION

echo "--- Pipelining ---"
run_bench "pipeline"      "$BASE_URL/"                  4 100 $DURATION 16

echo "--- Large Response ---"
run_bench "openapi"       "$BASE_URL/openapi.json"       4 100 $DURATION

echo "  ]" >> "$JSON_FILE"
echo "}" >> "$JSON_FILE"

if [ $SAVE_MODE -eq 1 ] || [ $CI_MODE -eq 1 ]; then
  cp "$JSON_FILE" "$RESULTS_DIR/latest.json"
  echo ""
  echo "Results saved to: $RESULTS_DIR/latest.json"
fi

if [ $COMPARE_MODE -eq 1 ] || [ $CI_MODE -eq 1 ]; then
  if [ -f "$RESULTS_DIR/baseline.json" ]; then
    echo ""
    echo "Comparing against baseline..."
    python3 "$SCRIPT_DIR/compare_benchmarks.py" \
      "$RESULTS_DIR/baseline.json" \
      "$JSON_FILE" \
      --threshold "$THRESHOLD" \
      --markdown "$RESULTS_DIR/comparison.md"
    
    COMPARE_EXIT=$?
    if [ $CI_MODE -eq 1 ] && [ $COMPARE_EXIT -ne 0 ]; then
      echo "ERROR: Performance regression detected!"
      exit $COMPARE_EXIT
    fi
  else
    echo "WARNING: No baseline.json found to compare."
  fi
fi

echo ""
echo "Benchmark completed."
