# stepwright development image: pinned Linux toolchain used for every local
# build/test run (via `make ...`) and by the Linux CI jobs.
#
#   docker build -t stepwright-dev -f docker/Dockerfile docker
#
# Toolchain (Ubuntu 24.04 archive packages only):
#   GCC 13/14, Clang 18/20 (+ libc++ 20, compiler-rt sanitizers), clang-format 20,
#   clang-tidy 20, CMake 3.28, Ninja, lcov, cppcheck, valgrind, Python 3, Doxygen.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG GCC_VERSION=14
ARG LLVM_VERSION=20

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates git make ninja-build cmake python3 \
        gcc-${GCC_VERSION} g++-${GCC_VERSION} gcc-13 g++-13 clang-18 \
        clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} \
        libclang-rt-${LLVM_VERSION}-dev \
        libc++-${LLVM_VERSION}-dev libc++abi-${LLVM_VERSION}-dev \
        clang-format-${LLVM_VERSION} clang-tidy-${LLVM_VERSION} \
        lcov cppcheck valgrind doxygen graphviz pkg-config libspdlog-dev \
 && rm -rf /var/lib/apt/lists/*

# Stable tool names regardless of the pinned versions.
RUN set -eux; \
    for t in gcc g++ gcov; do \
        ln -sf /usr/bin/${t}-${GCC_VERSION} /usr/local/bin/${t}; \
    done; \
    for t in clang clang++ clang-format clang-tidy run-clang-tidy llvm-cov llvm-symbolizer; do \
        ln -sf /usr/bin/${t}-${LLVM_VERSION} /usr/local/bin/${t}; \
    done

ENV STEPWRIGHT_IN_CONTAINER=1 \
    ASAN_SYMBOLIZER_PATH=/usr/local/bin/llvm-symbolizer

# Build trees and fetched dependencies live in a named volume mounted here;
# a fresh volume inherits these permissions so any UID can write to it.
RUN mkdir -p /work/build && chmod 0777 /work/build

WORKDIR /work/src
