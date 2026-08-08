# cmake/tooling.cmake — clang-format, clang-tidy, doxygen, coverage, benchmarks, profiling

# ── clang-format ──────────────────────────────────────────────────────────
find_program(CLANG_FORMAT_EXECUTABLE clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXECUTABLE}")

    file(GLOB_RECURSE CSILK_FORMAT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    )

    add_custom_target(format
        COMMAND ${CMAKE_COMMAND}
            -DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/format.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting C/H files with clang-format..."
    )

    add_custom_target(check-format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${CSILK_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking code formatting (dry-run)..."
    )

    add_compile_options(-Werror=implicit-function-declaration)
endif()

# ── Doxygen ───────────────────────────────────────────────────────────────
find_program(DOXYGEN_EXECUTABLE doxygen)
find_program(CURL_EXECUTABLE curl)
if(DOXYGEN_EXECUTABLE)
    set(MERMAID_JS "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets/mermaid.min.js")
    set(MERMAID_URL "https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js")
    if(CURL_EXECUTABLE)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets"
            COMMAND ${CURL_EXECUTABLE} -fsSL --connect-timeout 10 --retry 3 -o "${MERMAID_JS}" "${MERMAID_URL}"
            COMMAND ${CMAKE_COMMAND} -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" -DBIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" -DCSILK_VERSION="${CSILK_VERSION}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-doxyfile.cmake
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (Mermaid diagrams enabled)"
        )
        message(STATUS "Found curl: ${CURL_EXECUTABLE}")
    else()
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" -DBIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" -DCSILK_VERSION="${CSILK_VERSION}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-doxyfile.cmake
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (curl not found, Mermaid may be disabled)"
        )
        message(WARNING "curl not found — Mermaid diagrams may not render in Doxygen output")
    endif()
endif()

# ── clang-tidy ────────────────────────────────────────────────────────────
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE}
            --quiet
            --warnings-as-errors="*"
            --extra-arg=-Wno-everything
            -p ${CMAKE_CURRENT_BINARY_DIR}
            ${CSILK_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files (using .clang-tidy config)"
    )
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
else()
    message(STATUS "clang-tidy not found — skipping")
endif()

# ── Code coverage ─────────────────────────────────────────────────────────
if(USE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)
    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR}
                --filter "src/"
                --exclude "tests/"
                --exclude "examples/"
                --exclude "build_release/"
                --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
                --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS run_tests
            COMMENT "Generating code coverage report"
        )
        add_custom_target(coverage_summary
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR}
                --filter "src/"
                --exclude "tests/"
                --exclude "examples/"
                --exclude "build_release/"
                --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS run_tests
            COMMENT "Printing coverage summary"
        )
    else()
        message(WARNING "gcovr not found — install with 'pip install gcovr' for coverage reports")
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "gcovr not found — tests run without coverage report"
        )
    endif()
endif()

# ── Mermaid diagram validation ────────────────────────────────────────────
find_program(PYTHON3_EXECUTABLE python3)
if(PYTHON3_EXECUTABLE)
    add_custom_target(check-mermaid
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mermaid.py
            ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Validating Mermaid diagrams in documentation"
    )
    message(STATUS "Found python3: ${PYTHON3_EXECUTABLE}")
else()
    message(STATUS "python3 not found — skipping Mermaid validation target")
endif()

# ── Benchmark & profiling ─────────────────────────────────────────────────
add_custom_target(bench
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_benchmarks.sh --save
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running performance benchmarks with wrk"
)

add_custom_target(profile
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/profile.sh --duration 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Profiling example_server with perf + FlameGraph"
)
