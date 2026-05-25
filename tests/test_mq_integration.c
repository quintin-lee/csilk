#include "csilk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    
    assert(server != NULL);
    
    csilk_mq_t* mq = csilk_server_get_mq(server);
    assert(mq != NULL);
    
    printf("MQ instance successfully retrieved from server\n");
    
    csilk_server_free(server);
    csilk_router_free(router);
    
    printf("Server and MQ successfully freed\n");
    
    return 0;
}
