#!/bin/bash

# Build script for the example server

echo "Building example server..."

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Configure with CMake
cmake ..

# Build the example
make example_server

# If the above doesn't work, try direct compilation
if [ ! -f example_server ]; then
    echo "Trying direct compilation..."
    gcc -o example_server ../example_server.c \
        -I../include \
        src/context.c src/router.c src/group.c src/server.c src/url_parser.c \
        src/middleware/logger.c src/middleware/recovery.c src/middleware/auth.c src/middleware/static.c \
        -luv -llhttp -lcjson
fi

echo "Build complete. Run with: ./example_server"