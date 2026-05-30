# gen-doxyfile.cmake — Generate Doxyfile with correct version number.
# Invoked by the 'docs' custom target.
# Requires: -DCSILK_VERSION=<ver> on the cmake -P command line.

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
  "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
  @ONLY
)
message(STATUS "Generated Doxyfile with version ${CSILK_VERSION}")
