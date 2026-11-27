vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO quintin-lee/csilk
    REF v${VERSION}
    SHA512 0
    HEAD_REF main
)

set(OPTIONS)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic" OR "shared" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_BUILD_SHARED=ON)
else()
    list(APPEND OPTIONS -DCSILK_BUILD_SHARED=OFF)
endif()

if("full" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_BUILD_FULL=ON)
else()
    list(APPEND OPTIONS -DCSILK_BUILD_FULL=OFF)
endif()

# Handle optional domain modules and database drivers via features
if("db" IN_LIST FEATURES
   OR "mysql" IN_LIST FEATURES
   OR "postgresql" IN_LIST FEATURES
   OR "mongodb" IN_LIST FEATURES
   OR "redis" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_DB=ON)
endif()
if("ai" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_AI=ON)
endif()
if("vector" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_VECTOR=ON)
endif()
if("mcp" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_MCP=ON)
endif()
if("workflow" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_WORKFLOW=ON)
endif()
if("mq" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_MQ=ON)
endif()
if("grpc" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_GRPC=ON)
endif()
if("wasm" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_WASM=ON)
endif()
if("perm" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_PERM=ON)
endif()
if("observability" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_OBSERVABILITY=ON)
endif()
if("csv" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCSILK_ENABLE_CSV=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${OPTIONS}
        -DCSILK_BUILD_TESTS=OFF
        -DCSILK_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME csilk CONFIG_PATH lib/cmake/csilk)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
