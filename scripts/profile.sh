#!/usr/bin/env bash
#
# Profile the csilk example_server with perf + FlameGraph.
# Outputs: perf.data, flamegraph.svg, and a folded stack file.
#
# Usage: ./scripts/profile.sh [--duration SEC] [--rate RATE] [--output DIR]
#
# Prerequisites: perf (Linux), wrk, FlameGraph (tools/FlameGraph/).
#   Run ./scripts/setup_bench_tools.sh first.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-profile"
DURATION=30
RATE=50000  # wrk rate limit (requests/sec)
OUTPUT_DIR="${ROOT_DIR}/profile-output"
SERVER_PORT=8081
SERVER_BINARY=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift ;;
        --rate)    RATE="$2"; shift ;;
        --output)  OUTPUT_DIR="$2"; shift ;;
        *) echo "Usage: $0 [--duration SEC] [--rate RATE] [--output DIR]"; exit 1 ;;
    esac
    shift
done

FLAMEGRAPH_DIR="${ROOT_DIR}/tools/FlameGraph"
FLAMEGRAPH_PL="${FLAMEGRAPH_DIR}/flamegraph.pl"
STACKCOLLAPSE="${FLAMEGRAPH_DIR}/stackcollapse-perf.pl"

# ── Prerequisites check ─────────────────────────────────────────
if ! command -v perf &>/dev/null; then
    echo "Error: perf not found. Run ./scripts/setup_bench_tools.sh"
    exit 1
fi
if ! command -v wrk &>/dev/null; then
    echo "Error: wrk not found. Run ./scripts/setup_bench_tools.sh"
    exit 1
fi
if [[ ! -f "$FLAMEGRAPH_PL" ]]; then
    echo "Error: FlameGraph not found at $FLAMEGRAPH_DIR."
    echo "  Run ./scripts/setup_bench_tools.sh"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# ── Build example_server if needed ──────────────────────────────
if [[ ! -f "${BUILD_DIR}/example_server" ]]; then
    echo "=== Building example_server (Release, with frame pointers) ==="
    cmake -B "$BUILD_DIR" -S "$ROOT_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-fno-omit-frame-pointer" \
        -DENABLE_OOM_TEST=OFF
    cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" --target example_server
fi
SERVER_BINARY="${BUILD_DIR}/example_server"

# ── Generate server config ──────────────────────────────────────
CONFIG_FILE="${BUILD_DIR}/profile-config.yaml"
cat > "$CONFIG_FILE" << 'YAML'
port: 8081
server:
  max_header_size: 8192
  max_body_size: 1048576
  idle_timeout_ms: 30000
  listen_backlog: 512
YAML

# ── Start server ────────────────────────────────────────────────
echo "=== Starting server on port ${SERVER_PORT} ==="
echo "  Config: ${CONFIG_FILE}"
"$SERVER_BINARY" "$CONFIG_FILE" &
SERVER_PID=$!
sleep 2

# Verify server is running
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "Error: server failed to start"
    exit 1
fi

# ── Profile with perf ──────────────────────────────────────────
PERF_OUTPUT="${OUTPUT_DIR}/perf.data"
FOLDED_OUTPUT="${OUTPUT_DIR}/folded.stacks"
SVG_OUTPUT="${OUTPUT_DIR}/flamegraph.svg"

echo "=== Profiling with perf (${DURATION}s) ==="
echo "  Workload: wrk -c 100 -t 4 -R ${RATE} http://localhost:${SERVER_PORT}/"
echo "  Output:   ${SVG_OUTPUT}"

# Start perf record
perf record -g -p "$SERVER_PID" -o "$PERF_OUTPUT" --sleep "${DURATION}" &
PERF_PID=$!

# Give perf a moment to attach
sleep 1

# Generate load with wrk
wrk -c 100 -t 4 -R "$RATE" -d "${DURATION}" "http://localhost:${SERVER_PORT}/" 2>/dev/null || true

# Wait for perf to finish
wait "$PERF_PID" 2>/dev/null || true

# ── Generate flame graph ────────────────────────────────────────
echo "=== Generating flame graph ==="

# perf script → collapsed stacks → flamegraph SVG
perf script -i "$PERF_OUTPUT" 2>/dev/null \
    | "$STACKCOLLAPSE" 2>/dev/null \
    > "$FOLDED_OUTPUT" \
    || {
        echo "[warn] perf script failed — check kernel.perf_event_paranoid"
        echo "  Try: sudo sysctl kernel.perf_event_paranoid=1"
    }

if [[ -s "$FOLDED_OUTPUT" ]]; then
    "$FLAMEGRAPH_PL" "$FOLDED_OUTPUT" > "$SVG_OUTPUT"
    echo "[ok] Flame graph: ${SVG_OUTPUT}"
else
    echo "[warn] No stack samples collected; skipping flamegraph SVG"
fi

# ── Cleanup ─────────────────────────────────────────────────────
echo "=== Cleaning up ==="
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

echo ""
echo "=== Profiling complete ==="
echo "  perf data:  ${PERF_OUTPUT}"
echo "  folded:     ${FOLDED_OUTPUT}"
echo "  flamegraph: ${SVG_OUTPUT}"
echo ""
echo "Open ${SVG_OUTPUT} in a browser to view the flame graph."
