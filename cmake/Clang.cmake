# Clang / AppleClang flags and options for stepwright.
#
# stepwright is a small header-only C++20 library with no third-party runtime
# deps (only googletest/spdlog/benchmark via FetchContent), so this file
# stays minimal and macOS-aware.
#
# Two categories of flags:
#   1. ABI-affecting flags (sanitizers, coverage, hardening) -- applied to
#      CMAKE_CXX_FLAGS / CMAKE_EXE_LINKER_FLAGS so every translation unit
#      (including FetchContent dependencies) sees them. Mixing instrumented
#      and non-instrumented code is unsafe.
#   2. Warning flags (-Wall, -Wextra, -Werror, ...) -- exposed via the list
#      variable STEPWRIGHT_WARNING_FLAGS so they only apply to stepwright sources,
#      not to googletest/spdlog/benchmark.

message(STATUS "stepwright: using Clang configuration (${CMAKE_CXX_COMPILER_ID})")

option(STEPWRIGHT_WERROR "Treat warnings as errors for stepwright sources" ON)

set(STEPWRIGHT_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -fno-omit-frame-pointer
    -ferror-limit=3
    CACHE INTERNAL "Per-target warning flags for stepwright")
if(STEPWRIGHT_WERROR)
    list(APPEND STEPWRIGHT_WARNING_FLAGS -Werror)
endif()

# Hardening: FORTIFY_SOURCE in release, stack protector in debug.
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -D_FORTIFY_SOURCE=2")
string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fstack-protector-all")

# Linker hardening only on ELF/Linux. Apple's ld64 does not understand -z.
if(NOT APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -pie -Wl,-z,relro,-z,now")
endif()

# Opt-in: tune for the build machine (benchmarks only; binaries become
# non-portable).
option(STEPWRIGHT_NATIVE_ARCH "Compile stepwright's own targets with -march=native" OFF)
if(STEPWRIGHT_NATIVE_ARCH)
    string(APPEND CMAKE_CXX_FLAGS " -march=native")
endif()

# CODE_INSTRUMENTATION knob. One of: COVERAGE, ASAN, TSAN, UBSAN, LEAK.
if(CODE_INSTRUMENTATION)
    string(TOUPPER "${CODE_INSTRUMENTATION}" CODE_INSTRUMENTATION)
    message(STATUS "stepwright: instrumentation = ${CODE_INSTRUMENTATION}")

    if(CODE_INSTRUMENTATION STREQUAL "COVERAGE")
        # gcov-compatible output; works with scripts/coverage.py via the
        # llvm-gcov shim it generates on macOS.
        string(APPEND CMAKE_CXX_FLAGS " --coverage -fprofile-arcs -ftest-coverage")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " --coverage")
    elseif(CODE_INSTRUMENTATION STREQUAL "ASAN")
        string(APPEND CMAKE_CXX_FLAGS " -fsanitize=address,undefined -fno-sanitize-recover=all")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address,undefined")
    elseif(CODE_INSTRUMENTATION STREQUAL "UBSAN")
        string(APPEND CMAKE_CXX_FLAGS " -fsanitize=undefined -fno-sanitize-recover=all")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=undefined")
    elseif(CODE_INSTRUMENTATION STREQUAL "TSAN")
        string(APPEND CMAKE_CXX_FLAGS " -fsanitize=thread")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=thread")
    elseif(CODE_INSTRUMENTATION STREQUAL "LEAK")
        if(APPLE)
            # Standalone LSan is unavailable in AppleClang; ASan provides
            # leak detection on macOS when ASAN_OPTIONS=detect_leaks=1.
            message(WARNING
                "stepwright: LEAK on AppleClang falls back to ASan "
                "(set ASAN_OPTIONS=detect_leaks=1 at run-time).")
            string(APPEND CMAKE_CXX_FLAGS " -fsanitize=address")
            string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address")
        else()
            string(APPEND CMAKE_CXX_FLAGS " -fsanitize=leak")
            string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=leak")
        endif()
    else()
        message(FATAL_ERROR
            "Unknown CODE_INSTRUMENTATION='${CODE_INSTRUMENTATION}'. "
            "Expected COVERAGE | ASAN | TSAN | UBSAN | LEAK.")
    endif()
endif()
