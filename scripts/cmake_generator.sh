#!/usr/bin/env bash
# Sourced by ci.sh and its offline regression test. CMake keeps a tree's cached
# generator when -G is omitted; never replace or rewrite an existing cache.
stepwright_select_generator() {
    GENERATOR_ARGS=()
    if [[ ! -f "$1/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
        GENERATOR_ARGS=(-G Ninja)
    fi
}
