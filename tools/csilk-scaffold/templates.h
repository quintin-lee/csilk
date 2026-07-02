#ifndef CSILK_TEMPLATES_H
#define CSILK_TEMPLATES_H

const char* CMAKE_TMPL_FC =
    "cmake_minimum_required(VERSION 3.16)\n"
    "project(%s C)\n\n"
    "set(CMAKE_C_STANDARD 23)\n"
    "set(CMAKE_C_STANDARD_REQUIRED ON)\n\n"
    "include(FetchContent)\n"
    "FetchContent_Declare(\n"
    "    csilk\n"
    "    GIT_REPOSITORY https://github.com/quintin-lee/csilk.git\n"
    "    GIT_TAG master\n"
    ")\n"
    "FetchContent_MakeAvailable(csilk)\n\n"
    "file(GLOB_RECURSE SOURCES src/*.c)\n\n"
    "add_executable(%s ${SOURCES})\n"
    "target_link_libraries(%s PRIVATE csilk)\n"
    "FetchContent_GetProperties(csilk)\n"
    "target_include_directories(%s PRIVATE src include ${csilk_BINARY_DIR})\n"
    "configure_file(config.yaml config.yaml COPYONLY)\n";

const char* CMAKE_TMPL_LOCAL =
    "cmake_minimum_required(VERSION 3.16)\n"
    "project(%s C)\n\n"
    "set(CMAKE_C_STANDARD 23)\n"
    "set(CMAKE_C_STANDARD_REQUIRED ON)\n\n"
    "add_subdirectory(%s csilk_build)\n\n"
    "file(GLOB_RECURSE SOURCES src/*.c)\n\n"
    "add_executable(%s ${SOURCES})\n"
    "target_link_libraries(%s PRIVATE csilk)\n"
    "target_include_directories(%s PRIVATE src include %s/include %s/build)\n"
    "configure_file(config.yaml config.yaml COPYONLY)\n";

const char* CONFIG_TMPL = "port: %d\n"
              "server:\n"
              "  worker_threads: 4\n"
              "logger:\n"
              "  level: INFO\n"
              "middleware:\n"
              "  enable_recovery: %d\n"
              "  enable_logger: %d\n"
              "  enable_gzip: %d\n"
              "  enable_waf: %d\n"
              "  enable_ratelimit: %d\n"
              "static_files:\n"
              "  enable: 1\n"
              "  root_dir: \"./public\"\n"
              "  prefix: \"/static\"\n";

const char* MAIN_C_LL = "#include \"csilk/csilk.h\"\n\n"
            "%s" // Handlers
            "int main(void) {\n"
            "    %s" // Init calls
            "    csilk_router_t* r = csilk_router_new();\n"
            "    %s" // Routing logic
            "    csilk_server_t* s = csilk_server_new(r);\n"
            "    %s" // Server options
            "    return csilk_server_run(s, %d);\n"
            "}\n";

const char* MAIN_C_APP = "#include \"csilk/csilk.h\"\n"
             "#include \"csilk/app/app.h\"\n\n"
             "%s" // Handlers
             "int main(void) {\n"
             "    %s" // Init calls
             "    csilk_app_t* app = csilk_app_new(\"config.yaml\");\n"
             "    %s" // Routing & middleware
             "    %s" // App features (admin, prometheus, swagger)
             "    return csilk_app_run(app, %d);\n"
             "}\n";

#endif
