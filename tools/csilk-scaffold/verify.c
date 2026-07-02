#include "scaffold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void generate_project(const project_config_t* cfg, const char* base_path);

int
verify_project(const project_config_t* cfg)
{
    char tmp_dir[] = "/tmp/csilk-scaffold-XXXXXX";
    if (!mkdtemp(tmp_dir)) {
        return -1;
    }

    printf("Verifying in sandbox: %s\n", tmp_dir);
    generate_project(cfg, tmp_dir);

    char cmd[1024];
    int res;

    printf("Running CMake...\n");
    snprintf(
        cmd, sizeof(cmd), "cmake -B %s/build -S %s -DCMAKE_BUILD_TYPE=Debug", tmp_dir, tmp_dir);
    res = system(cmd);
    if (res != 0) {
        printf("Verification Failed: CMake error\n");
        return -1;
    }

    printf("Running Make...\n");
    snprintf(cmd, sizeof(cmd), "cmake --build %s/build -j4", tmp_dir);
    res = system(cmd);
    if (res != 0) {
        printf("Verification Failed: Compilation error\n");
        return -1;
    }

    printf("✓ Verification Successful!\n");

    // Cleanup sandbox (optional, but good for test)
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_dir);
    system(cmd);

    return 0;
}
