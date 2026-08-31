# CMake source ownership contract for phase-one module boundaries.

if(NOT DEFINED CSILK_SOURCE_DIR)
  message(FATAL_ERROR "CSILK_SOURCE_DIR must point at the project root")
endif()

file(READ "${CSILK_SOURCE_DIR}/cmake/sources.cmake" _sources)

foreach(_variable
    CSILK_CRYPTO_SOURCES
    CSILK_RUNTIME_SOURCES
    CSILK_PROTOCOLS_SOURCES
    CSILK_MIDDLEWARE_SOURCES
    CSILK_REFLECTION_SOURCES
    CSILK_PERMISSION_SOURCES
    CSILK_APP_SOURCES
    CSILK_VECTOR_SOURCES)
  if(NOT _sources MATCHES "set\\(${_variable}")
    message(FATAL_ERROR "missing source ownership variable: ${_variable}")
  endif()
endforeach()

string(REGEX MATCH "set\\(CSILK_HTTP_SOURCES[^)]*\\)" _http_block "${_sources}")
if(NOT _http_block)
  message(FATAL_ERROR "missing CSILK_HTTP_SOURCES block")
endif()
foreach(_forbidden
    "src/core/ctx/context.c"
    "src/core/server/connection.c"
    "src/middleware/gzip.c"
    "src/reflection/reflect.c"
    "src/drivers/vector/vector.c"
    "src/core/test_utils.c")
  if(_http_block MATCHES "${_forbidden}")
    message(FATAL_ERROR "${_forbidden} is still owned by CSILK_HTTP_SOURCES")
  endif()
endforeach()

string(REGEX MATCH "set\\(CSILK_DB_SOURCES[^)]*\\)" _db_block "${_sources}")
if(_db_block MATCHES "src/drivers/vector/")
  message(FATAL_ERROR "vector sources are still owned by CSILK_DB_SOURCES")
endif()

string(REGEX MATCH "set\\(CSILK_CORE_SOURCES[^)]*\\)" _core_block "${_sources}")
if(_core_block MATCHES "src/crypto/|src/core/server/|src/core/ctx/|src/core/config/")
  message(FATAL_ERROR "implementation sources are still owned by compatibility CSILK_CORE_SOURCES")
endif()

message(STATUS "phase-one source ownership contract passed")
