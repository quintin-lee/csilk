vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO quintin-lee/csilk
    REF v${VERSION}
    SHA512 0
    HEAD_REF main
)

set(OPTIONS)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    list(APPEND OPTIONS -DCSILK_BUILD_SHARED=ON)
endif()

# Handle optional database drivers via features
if("mysql" IN_LIST FEATURES)
    list(APPEND OPTIONS -DHAS_MYSQL=ON)
endif()
if("postgresql" IN_LIST FEATURES)
    list(APPEND OPTIONS -DHAS_POSTGRES=ON)
endif()
if("mongodb" IN_LIST FEATURES)
    list(APPEND OPTIONS -DHAS_MONGODB=ON)
endif()
if("redis" IN_LIST FEATURES)
    list(APPEND OPTIONS -DHAS_REDIS=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${OPTIONS}
        -DBUILD_TESTING=OFF
        -DBUILD_EXAMPLES=OFF
    MAYBE_UNUSED_VARIABLES
        HAS_MYSQL
        HAS_POSTGRES
        HAS_MONGODB
        HAS_REDIS
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME csilk CONFIG_PATH lib/cmake/csilk)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
