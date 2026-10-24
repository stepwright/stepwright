# Third-party dependencies, pinned by version and SHA-256.
# Only stepwright's own tests/benchmarks/logging use these; the library itself
# has no runtime dependencies.

include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

macro(stepwright_fetch_googletest)
    if(NOT TARGET GTest::gtest_main)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
            URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endmacro()

macro(stepwright_fetch_benchmark)
    if(NOT TARGET benchmark::benchmark)
        FetchContent_Declare(benchmark
            URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
            URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(benchmark)
    endif()
endmacro()

macro(stepwright_fetch_spdlog)
    if(NOT TARGET spdlog::spdlog)
        find_package(spdlog QUIET)
        if(NOT spdlog_FOUND)
            FetchContent_Declare(spdlog
                URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.tar.gz
                URL_HASH SHA256=9962648c9b4f1a7bbc76fd8d9172555bad1871fdb14ff4f842ef87949682caa5)
            set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(spdlog)
        endif()
    endif()
endmacro()
