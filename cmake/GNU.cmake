# GCC flags and options for stepwright.
#
# See cmake/Clang.cmake for the rationale on the warning-vs-ABI split.

message(STATUS "stepwright: using GCC configuration")

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
    -Wbuiltin-macro-redefined
    -fno-omit-frame-pointer
    -fmax-errors=3
    CACHE INTERNAL "Per-target warning flags for stepwright")
if(STEPWRIGHT_WERROR)
    list(APPEND STEPWRIGHT_WARNING_FLAGS -Werror)
endif()

string(APPEND CMAKE_CXX_FLAGS_RELEASE " -D_FORTIFY_SOURCE=2")
string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fstack-protector-all")

# ELF link hardening (Apple's ld64 does not understand -z).
if(NOT APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -pie -Wl,-z,relro,-z,now")
endif()

# Opt-in: tune for the build machine (benchmarks only; binaries become
# non-portable).
option(STEPWRIGHT_NATIVE_ARCH "Compile stepwright's own targets with -march=native" OFF)
if(STEPWRIGHT_NATIVE_ARCH)
    string(APPEND CMAKE_CXX_FLAGS " -march=native")
endif()

if(CODE_INSTRUMENTATION)
    string(TOUPPER "${CODE_INSTRUMENTATION}" CODE_INSTRUMENTATION)
    message(STATUS "stepwright: instrumentation = ${CODE_INSTRUMENTATION}")

    if(CODE_INSTRUMENTATION STREQUAL "COVERAGE")
        string(APPEND CMAKE_CXX_FLAGS " -g -O0 --coverage")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -g --coverage")
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
        string(APPEND CMAKE_CXX_FLAGS " -fsanitize=leak")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=leak")
    else()
        message(FATAL_ERROR
            "Unknown CODE_INSTRUMENTATION='${CODE_INSTRUMENTATION}'. "
            "Expected COVERAGE | ASAN | TSAN | UBSAN | LEAK.")
    endif()
endif()
