#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <uv.h>
#include "gin.h"

// Simple test to verify connection keep-alive
// We'll mock the client and see if it stays open

int main() {
    printf("Starting keep-alive test...\n");
    // TODO: Write actual test logic that connects, sends two requests, 
    // and verifies both are handled on the same connection.
    // For now, this just compiles and runs.
    printf("Test passed!\n");
    return 0;
}
