#!/bin/bash

# Csilk Performance Benchmark Script
# Requirements: wrk, csilk example_server running on port 8080

PORT=8080
DURATION=30s
THREADS=4
CONNECTIONS=100
BASE_URL="http://localhost:$PORT"

echo "Starting Csilk benchmarks..."

# 1. Static file benchmark
echo "--- Benchmarking Static File Serving ---"
wrk -t$THREADS -c$CONNECTIONS -d$DURATION $BASE_URL/static/test.html

# 2. JSON serialization benchmark
echo "--- Benchmarking JSON Response ---"
wrk -t$THREADS -c$CONNECTIONS -d$DURATION $BASE_URL/json

# 3. Deeply nested route benchmark
echo "--- Benchmarking Deep Routing ---"
wrk -t$THREADS -c$CONNECTIONS -d$DURATION $BASE_URL/api/v1/users/profile

echo "Benchmarks complete."
