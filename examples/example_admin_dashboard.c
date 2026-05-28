/**
 * @file example_admin_dashboard.c
 * @brief Demonstrates the unified administration and monitoring dashboard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "csilk/csilk.h"
#include "csilk/app/admin.h"
#include "csilk/app/workflow.h"

void hello_handler(csilk_ctx_t* c) {
    csilk_string(c, 200, "Hello from Csilk!");
}

void slow_handler(csilk_ctx_t* c) {
    usleep(200000); // 200ms
    csilk_string(c, 200, "That was slow...");
}

void mq_handler(csilk_ctx_t* c) {
    csilk_mq_t* mq = csilk_server_get_mq(csilk_ctx_get_server(c));
    const char* msg = "User clicked a button";
    csilk_mq_publish(mq, "user.events", msg, strlen(msg));
    csilk_string(c, 200, "Event published to MQ");
}

static void tick_mq(uv_timer_t* h) {
    csilk_mq_t* mq = (csilk_mq_t*)h->data;
    csilk_mq_publish(mq, "system.tick", "ping", 4);
}

int main() {
    printf("Starting Csilk Admin Dashboard Example on port 8080...\n");
    printf("Access the dashboard at: http://localhost:8080/admin\n\n");
    
    csilk_app_t* app = csilk_app_new(NULL);
    
    /* 1. Enable Metrics Middleware Globally */
    csilk_app_use(app, csilk_metrics_middleware);
    
    /* 2. Setup Admin Dashboard */
    csilk_admin_serve(app, "/admin");
    
    /* 3. Register Sample Routes */
    csilk_app_get(app, "/", hello_handler);
    csilk_app_get(app, "/slow", slow_handler);
    csilk_app_get(app, "/pub", mq_handler);
    
    /* 4. Background MQ Activity */
    uv_timer_t mq_timer;
    uv_timer_init(uv_default_loop(), &mq_timer);
    mq_timer.data = csilk_server_get_mq(csilk_app_server(app));
    uv_timer_start(&mq_timer, tick_mq, 1000, 3000); // Every 3s

    /* 5. Start Server */
    csilk_app_run(app, 8080);
    
    return 0;
}
