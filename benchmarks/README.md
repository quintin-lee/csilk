# Csilk Benchmarks

This directory contains scripts and documentation for benchmarking the Csilk web framework.

## Prerequisites

- `wrk`: HTTP benchmarking tool. Install via `sudo apt install wrk`.
- `csilk` built with optimizations: `cmake .. -DCMAKE_BUILD_TYPE=Release && make`.

## Running Benchmarks

1. Start the example server:
   ```bash
   ./build/examples/example_server
   ```

2. In a separate terminal, run the benchmark script:
   ```bash
   ./benchmarks/run_benchmarks.sh
   ```

## Targets

- `/static/test.html`: Tests static file serving performance (now using zero-copy `sendfile`).
- `/json`: Tests JSON serialization and response overhead.
- `/api/v1/users/profile`: Tests Radix Tree router performance with deeply nested paths.

## Hardware Specifications

Please record your hardware specifications when reporting benchmark results.
