# AppleClang shares Clang's flags and CODE_INSTRUMENTATION dispatch.
# CMake's per-compiler include uses ${CMAKE_CXX_COMPILER_ID}, which is
# "AppleClang" on macOS even though the underlying compiler is clang.
include(${CMAKE_CURRENT_LIST_DIR}/Clang.cmake)
