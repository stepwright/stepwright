cmake_minimum_required(VERSION 3.21)
project(stepwright VERSION 0.2.1 LANGUAGES CXX)

# The checked-in header also supports direct -I consumers. Fail configuration
# rather than silently publishing two different versions.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/stepwright/version.hpp" stepwright_version_header)
foreach(part MAJOR MINOR PATCH)
    string(REGEX MATCH "#define STEPWRIGHT_VERSION_${part} ([0-9]+)" match "${stepwright_version_header}")
    if(NOT match OR NOT CMAKE_MATCH_1 STREQUAL "${PROJECT_VERSION_${part}}")
        message(FATAL_ERROR "stepwright version.hpp disagrees with project VERSION (${part})")
    endif()
endforeach()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Developer-only knobs (tests, examples, benchmarks, sanitizers, warnings) are
# enabled only when stepwright is the top-level project, so consumers that pull
# stepwright in via FetchContent / add_subdirectory never inherit them.
option(STEPWRIGHT_BUILD_TESTS "Build stepwright tests" ${PROJECT_IS_TOP_LEVEL})
option(STEPWRIGHT_BUILD_EXAMPLES "Build stepwright examples" ${PROJECT_IS_TOP_LEVEL})
option(STEPWRIGHT_BUILD_BENCHMARKS "Build stepwright benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(STEPWRIGHT_CLANG_TIDY "Run clang-tidy during the build" OFF)
option(STEPWRIGHT_ENABLE_LOGGING "Enable optional spdlog-backed logging" OFF)
# Changes the layout of machine/source_machine: applies to the whole program
# through the interface target (see include/stepwright/debug/live_tasks.hpp).
option(STEPWRIGHT_TRACK_TASKS "Track in-flight machines in the live task registry (debug)" OFF)
set(STEPWRIGHT_LOG_LEVEL "DEBUG" CACHE STRING
    "Compile-time log level when STEPWRIGHT_ENABLE_LOGGING=ON")
set_property(CACHE STEPWRIGHT_LOG_LEVEL PROPERTY STRINGS
    TRACE DEBUG INFO WARN ERROR CRITICAL OFF)

# One of: COVERAGE | ASAN | TSAN | UBSAN | LEAK (empty = ordinary build).
set(CODE_INSTRUMENTATION "" CACHE STRING
    "Optional build instrumentation: COVERAGE|ASAN|TSAN|UBSAN|LEAK")
set_property(CACHE CODE_INSTRUMENTATION PROPERTY STRINGS
    "" COVERAGE ASAN TSAN UBSAN LEAK)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/cmake/${CMAKE_CXX_COMPILER_ID}.cmake")
        include(cmake/${CMAKE_CXX_COMPILER_ID}.cmake)
    else()
        message(STATUS "stepwright: no cmake/${CMAKE_CXX_COMPILER_ID}.cmake, using defaults")
    endif()
    include(cmake/cppcheck.cmake)

    if(STEPWRIGHT_CLANG_TIDY)
        find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
    endif()
endif()

# ---------------------------------------------------------------------------
# The library: header-only.
# ---------------------------------------------------------------------------
add_library(stepwright INTERFACE)
add_library(stepwright::stepwright ALIAS stepwright)
target_include_directories(stepwright INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(stepwright INTERFACE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(stepwright INTERFACE Threads::Threads)

if(STEPWRIGHT_TRACK_TASKS)
    target_compile_definitions(stepwright INTERFACE STEPWRIGHT_TRACK_TASKS=1)
    message(STATUS "stepwright: live task registry ENABLED")
endif()

if(STEPWRIGHT_ENABLE_LOGGING)
    # Optional logging is a consumer dependency, never fetched behind their back.
    find_package(spdlog CONFIG REQUIRED)
    target_link_libraries(stepwright INTERFACE spdlog::spdlog)
    target_compile_definitions(stepwright INTERFACE
        STEPWRIGHT_ENABLE_LOGGING
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_${STEPWRIGHT_LOG_LEVEL})
    message(STATUS "stepwright: logging ENABLED (level ${STEPWRIGHT_LOG_LEVEL})")
endif()

# Header-only install. No tests, examples or benchmark dependencies are exported.
install(DIRECTORY include/stepwright DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS stepwright EXPORT stepwrightTargets)
install(EXPORT stepwrightTargets NAMESPACE stepwright:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/stepwright")
configure_package_config_file(cmake/stepwrightConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/stepwrightConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/stepwright")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/stepwrightConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/stepwrightConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/stepwrightConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/stepwright")
file(RELATIVE_PATH STEPWRIGHT_PC_PREFIX "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
set(STEPWRIGHT_PC_DEFINITIONS "")
set(STEPWRIGHT_PC_REQUIRES "")
if(STEPWRIGHT_TRACK_TASKS)
    string(APPEND STEPWRIGHT_PC_DEFINITIONS " -DSTEPWRIGHT_TRACK_TASKS=1")
endif()
if(STEPWRIGHT_ENABLE_LOGGING)
    string(APPEND STEPWRIGHT_PC_DEFINITIONS
        " -DSTEPWRIGHT_ENABLE_LOGGING -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_${STEPWRIGHT_LOG_LEVEL}")
    set(STEPWRIGHT_PC_REQUIRES "spdlog")
endif()
configure_file(cmake/stepwright.pc.in "${CMAKE_CURRENT_BINARY_DIR}/stepwright.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/stepwright.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stepwright")

# Warning flags for stepwright's own test/example/benchmark targets.
function(stepwright_apply_warnings target)
    if(STEPWRIGHT_WARNING_FLAGS)
        target_compile_options(${target} PRIVATE ${STEPWRIGHT_WARNING_FLAGS})
    endif()
endfunction()

if(STEPWRIGHT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(STEPWRIGHT_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(STEPWRIGHT_BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/benchmarks/CMakeLists.txt")
    add_subdirectory(benchmarks)
endif()
