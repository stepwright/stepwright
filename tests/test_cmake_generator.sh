#!/usr/bin/env bash
# Exercise the exact selection function used by ci.sh, offline and without a DB.
set -euo pipefail
source "$1"
work="$(mktemp -d "$2/generator-XXXXXX")"
original_path="$PATH"
trap 'PATH="$original_path"; rm -rf "$work"' EXIT
mkdir -p "$work/with-ninja" "$work/no-tools" "$work/cached"
printf '#!/bin/sh\nexit 0\n' > "$work/with-ninja/ninja"
chmod +x "$work/with-ninja/ninja"
printf 'CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n' > "$work/cached/CMakeCache.txt"

PATH="$work/with-ninja"
stepwright_select_generator "$work/new"
[[ "${GENERATOR_ARGS[*]-}" == '-G Ninja' ]]
stepwright_select_generator "$work/cached"
[[ -z "${GENERATOR_ARGS[*]-}" ]]
IFS= read -r cache_line < "$work/cached/CMakeCache.txt"
[[ "$cache_line" == 'CMAKE_GENERATOR:INTERNAL=Unix Makefiles' ]]

PATH="$work/no-tools"
stepwright_select_generator "$work/new"
[[ -z "${GENERATOR_ARGS[*]-}" ]]
stepwright_select_generator "$work/cached"
[[ -z "${GENERATOR_ARGS[*]-}" ]]
echo 'cached/new generator selection checks passed'
