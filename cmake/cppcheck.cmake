# Inline cppcheck integration. Enable with -DCPPCHECK=ON.
#
# Once enabled, CMake invokes cppcheck after each successful translation unit
# build (CMAKE_CXX_CPPCHECK), so warnings show up immediately during `make`.
# For a full project scan (including suppression-category reports), use
# scripts/run-cppcheck.sh instead.

option(CPPCHECK "Run cppcheck inline during compilation" OFF)

if(CPPCHECK)
    find_program(CPPCHECK_EXE cppcheck REQUIRED)
    message(STATUS "stepwright: cppcheck inline ENABLED (${CPPCHECK_EXE})")

    set(CPPCHECK_BUILD_DIR ${PROJECT_BINARY_DIR}/cppcheck)
    file(MAKE_DIRECTORY ${CPPCHECK_BUILD_DIR})

    set(CMAKE_CXX_CPPCHECK
        "${CPPCHECK_EXE}"
        "--enable=warning,performance,portability"
        "--error-exitcode=1"
        "--inline-suppr"
        "--quiet"
        "--std=c++20"
        "--library=googletest"
        "--cppcheck-build-dir=${CPPCHECK_BUILD_DIR}"
        "--suppressions-list=${CMAKE_SOURCE_DIR}/.cppcheck/suppressions.txt"
        CACHE STRING "cppcheck command line" FORCE)
endif()
