#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk.h"

void test_csrf_token_generation() {
    char token1[64];
    char token2[64];
    
    assert(csilk_csrf_generate_token(token1, sizeof(token1)) == 0);
    assert(csilk_csrf_generate_token(token2, sizeof(token2)) == 0);
    
    assert(strlen(token1) == 32);
    assert(strcmp(token1, token2) != 0); // Tokens should be unique
    
    printf("test_csrf_token_generation passed\n");
}

int main() {
    test_csrf_token_generation();
    return 0;
}
