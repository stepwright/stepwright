cmake_minimum_required(VERSION 3.21)
project(stepwright VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Developer-only knobs (tests, examples, benchmarks, sanitizers, warnings) are
# enabled only when stepwright is the top-level project, so consumers that pull
# stepwright in via FetchContent / add_subdirectory never inherit them.
option(STEPWRIGHT_BUILD_TESTS "Build stepwright tests" ${PROJECT_IS_TOP_LEVEL})
option(STEPWRIGHT_BUILD_EXAMPLES "Build stepwright examples" ${PROJECT_IS_TOP_LEVEL})
option(STEPWRIGHT_BUILD_BENCHMARKS "Build stepwright benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(STEPWRIGHT_CLANG_TIDY "Run clang-tidy during the build" OFF)
option(STEPWRIGHT_ENABLE_LOGGING "Enable spdlog-backed debug logging (dev only)" OFF)
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
    $<INSTALL_INTERFACE:include>
)
target_compile_features(stepwright INTERFACE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(stepwright INTERFACE Threads::Threads)

if(STEPWRIGHT_ENABLE_LOGGING)
    include(cmake/Dependencies.cmake)
    stepwright_fetch_spdlog()
    target_link_libraries(stepwright INTERFACE spdlog::spdlog)
    target_compile_definitions(stepwright INTERFACE
        STEPWRIGHT_ENABLE_LOGGING
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_${STEPWRIGHT_LOG_LEVEL})
    message(STATUS "stepwright: logging ENABLED (level ${STEPWRIGHT_LOG_LEVEL})")
endif()

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
