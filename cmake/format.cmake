# format.cmake - Run clang-format only on files that need formatting,
# preserving mtime of already-formatted files to avoid unnecessary rebuilds.

if(NOT CLANG_FORMAT)
  message(FATAL_ERROR "CLANG_FORMAT not set")
endif()

file(GLOB_RECURSE SOURCE_FILES
  "${SOURCE_DIR}/src/*.c"
  "${SOURCE_DIR}/src/*.h"
  "${SOURCE_DIR}/include/*.h"
  "${SOURCE_DIR}/tests/*.c"
  "${SOURCE_DIR}/tests/*.h"
  "${SOURCE_DIR}/examples/*.c"
  "${SOURCE_DIR}/examples/*.h"
)

foreach(F ${SOURCE_FILES})
  execute_process(
    COMMAND ${CLANG_FORMAT} --output-replacements-xml ${F}
    OUTPUT_VARIABLE REPLACEMENTS
    ERROR_QUIET
  )
  string(REGEX MATCH "<replacement " NEEDS_FORMAT "${REPLACEMENTS}")
  if(NEEDS_FORMAT)
    execute_process(COMMAND ${CLANG_FORMAT} -i ${F})
  endif()
endforeach()
