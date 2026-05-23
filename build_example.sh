#!/bin/bash

# Build script for example servers

echo "Building example servers..."

mkdir -p build
cd build

cmake ..

make example_server example_app

echo "Build complete."
echo "  Run: ./example_server"
echo "  Run: ./example_app"