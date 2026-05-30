# gen-doxyfile.cmake — Generate Doxyfile with correct version number.
# Invoked by the 'docs' custom target.
# Requires: -DSRC_DIR=<path> -DBIN_DIR=<path> -DCSILK_VERSION=<ver>

configure_file(
  "${SRC_DIR}/Doxyfile.in"
  "${BIN_DIR}/Doxyfile"
  @ONLY
)
message(STATUS "Generated Doxyfile with version ${CSILK_VERSION}")
