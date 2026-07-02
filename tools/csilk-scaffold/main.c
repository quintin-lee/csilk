#include "scaffold.h"
#include "tui.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void run_wizard(project_config_t* cfg);

int
main(int argc, char** argv)
{
    project_config_t cfg = {0};

    // Default config
    strcpy(cfg.name, "my_app");
    cfg.port = 8080;
    cfg.has_recovery = true;
    cfg.has_logger = true;

    // Detect local path for verification
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        // We assume we are running from tools/csilk-scaffold or root
        if (strstr(cwd, "/tools/csilk-scaffold")) {
            // Move 2 levels up
            char* p = strstr(cwd, "/tools/csilk-scaffold");
            *p = '\0';
            strcpy(cfg.local_csilk_path, cwd);
        } else {
            // Assume root
            strcpy(cfg.local_csilk_path, cwd);
        }
    }

    tui_init();
    run_wizard(&cfg);
    tui_cleanup();

    return 0;
}
