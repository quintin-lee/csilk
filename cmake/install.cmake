# cmake/install.cmake — install targets, CMake package config, pkg-config, and CPack packaging

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CSILK_INSTALL_TARGETS
    csilk
    csilk_core
    csilk_http
    csilk_tls
    csilk_http2
    csilk_db
    csilk_ai
    csilk_mq
    csilk_workflow
)
if(CSILK_USE_URING)
    list(APPEND CSILK_INSTALL_TARGETS uring)
endif()

install(TARGETS ${CSILK_INSTALL_TARGETS}
    EXPORT csilk-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CSILK_BUILD_SHARED)
    install(TARGETS csilk_shared
        EXPORT csilk-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# version.h is generated at configure time into the binary include/ tree,
# so it must be installed explicitly (the source include/ only holds the .in
# template).
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/csilk/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/csilk)
install(DIRECTORY share/swagger-ui/ DESTINATION ${CMAKE_INSTALL_DATADIR}/csilk/swagger-ui)

# ── CMake Package Configuration (csilk-targets.cmake & csilk-config.cmake) ──
install(EXPORT csilk-targets
    FILE csilk-targets.cmake
    NAMESPACE csilk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/csilk
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/csilk-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/csilk"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config-version.cmake"
    VERSION "${CSILK_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/csilk"
)

# ── pkg-config (csilk.pc) ────────────────────────────────────────────────
set(CSILK_PC_LIBS_PRIVATE "")
if(CSILK_USE_URING)
  string(APPEND CSILK_PC_LIBS_PRIVATE " -luring")
else()
  string(APPEND CSILK_PC_LIBS_PRIVATE " -luv")
endif()
string(APPEND CSILK_PC_LIBS_PRIVATE " -lssl -lcrypto -lcurl -lsqlite3 -lz -lyaml -lnghttp2")
if(NOT APPLE AND NOT WIN32)
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lm -lpthread")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/csilk.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/csilk.pc"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/csilk.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

# ── CPack Packaging ───────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "csilk")
set(CPACK_PACKAGE_VERSION "${CSILK_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Csilk")
include(CPack)
