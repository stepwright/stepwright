#!/usr/bin/env bash
# Single entry point for every build / test / quality task. Used:
#   * natively on macOS              (make <task>)
#   * inside the Linux dev container (make docker-<task>)
#   * by CI                          (.github/workflows/*)
#
# usage: scripts/ci.sh <task> [extra args]
#
# Tasks:
#   build | test | debug | asan | tsan | ubsan | leak | bench | run
#   format | format-check | tidy | cppcheck | forbidden | coverage | check
#
# Environment:
#   COMPILER    native | gcc | clang | clang-libcxx   (default: native, or gcc in the container)
#   BUILD_TYPE  Release | Debug | RelWithDebInfo      (build/test/bench; default Release)
#   BUILD_ROOT  where build trees live                (default: <repo>/build)
#   JOBS        parallelism                           (default: CPU count)
#   EXAMPLE     example to run for `run`
#   BENCH_ARGS  extra Google Benchmark arguments
#   CMAKE_ARGS  extra CMake configure arguments
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

TASK="${1:-}"
[[ -n "${TASK}" ]] || { sed -n '2,25p' "$0"; exit 2; }
shift || true

OS="$(uname -s)"
if [[ -n "${STEPWRIGHT_IN_CONTAINER:-}" ]]; then
    COMPILER="${COMPILER:-gcc}"
else
    COMPILER="${COMPILER:-native}"
fi
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_ROOT="${BUILD_ROOT:-${REPO_ROOT}/build}"
JOBS="${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"
CMAKE_ARGS="${CMAKE_ARGS:-}"
BENCH_ARGS="${BENCH_ARGS:---benchmark_min_time=1s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true}"

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
log() { printf '\033[1;36m>>> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Compiler selection
# ---------------------------------------------------------------------------
COMPILER_ARGS=()
select_compiler() {
    case "${COMPILER}" in
        native) ;;
        gcc)
            local cxx=""
            for c in g++ g++-16 g++-15 g++-14 g++-13; do
                if command -v "${c}" >/dev/null 2>&1 && "${c}" --version 2>/dev/null | grep -q 'Free Software'; then
                    cxx="${c}"; break
                fi
            done
            [[ -n "${cxx}" ]] || die "no GNU g++ found"
            COMPILER_ARGS+=("-DCMAKE_CXX_COMPILER=${cxx}")
            ;;
        clang)
            COMPILER_ARGS+=("-DCMAKE_CXX_COMPILER=clang++")
            ;;
        clang-libcxx)
            COMPILER_ARGS+=("-DCMAKE_CXX_COMPILER=clang++" "-DCMAKE_CXX_FLAGS=-stdlib=libc++")
            ;;
        *) die "unknown COMPILER='${COMPILER}' (native|gcc|clang|clang-libcxx)" ;;
    esac
}

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

# configure <variant> <build_type> [cmake args...]  -> sets BUILD_DIR
configure() {
    local variant="$1" build_type="$2"; shift 2
    # Different CMAKE_ARGS (e.g. -DSTEPWRIGHT_TRACK_TASKS=ON) must never share a build
    # tree: CMake caches options, so a later run without them would silently reuse them.
    local suffix=""
    if [[ -n "${CMAKE_ARGS}" ]]; then
        suffix="-$(printf '%s' "${CMAKE_ARGS}" | cksum | cut -d' ' -f1)"
    fi
    BUILD_DIR="${BUILD_ROOT}/${COMPILER}-${variant}${suffix}"
    select_compiler
    log "configure ${BUILD_DIR} (${build_type}, compiler=${COMPILER})"
    # shellcheck disable=SC2086
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DFETCHCONTENT_QUIET=ON \
        ${COMPILER_ARGS[@]+"${COMPILER_ARGS[@]}"} "$@" ${CMAKE_ARGS}
}

build() {
    log "build ${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" -j "${JOBS}" "$@"
}

run_tests() {
    log "ctest ${BUILD_DIR}"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure --timeout 300 -j "${JOBS}"
}

sanitizer() {
    local kind="$1"
    configure "$(lower "${kind}")" Debug -DCODE_INSTRUMENTATION="${kind}" -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF
    build
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
    export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"
    if [[ "${OS}" == "Linux" ]]; then
        export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1"
    else
        # LeakSanitizer is not available on macOS/arm64.
        export ASAN_OPTIONS="abort_on_error=1:strict_string_checks=1"
    fi
    run_tests
}

# ---------------------------------------------------------------------------
# Tasks
# ---------------------------------------------------------------------------
case "${TASK}" in
    build)
        configure "$(lower "${BUILD_TYPE}")" "${BUILD_TYPE}"
        build
        ;;
    test)
        configure "$(lower "${BUILD_TYPE}")" "${BUILD_TYPE}"
        build
        run_tests
        ;;
    debug)
        configure debug Debug
        build
        run_tests
        ;;
    asan)  sanitizer ASAN ;;
    tsan)  sanitizer TSAN ;;
    ubsan) sanitizer UBSAN ;;
    leak)
        [[ "${OS}" == "Linux" ]] || die "LeakSanitizer needs Linux: use 'make docker-leak'"
        sanitizer LEAK
        ;;
    bench)
        configure bench Release -DSTEPWRIGHT_BUILD_TESTS=OFF -DSTEPWRIGHT_BUILD_EXAMPLES=OFF
        build
        found=0
        for b in "${BUILD_DIR}"/benchmarks/bench_*; do
            [[ -x "${b}" && -f "${b}" ]] || continue
            found=1
            log "$(basename "${b}")"
            # shellcheck disable=SC2086
            "${b}" ${BENCH_ARGS} "$@"
        done
        [[ ${found} -eq 1 ]] || log "no benchmarks built yet"
        ;;
    run)
        [[ -n "${EXAMPLE:-}" ]] || die "set EXAMPLE=<name>"
        configure "$(lower "${BUILD_TYPE}")" "${BUILD_TYPE}"
        build --target "${EXAMPLE}"
        "${BUILD_DIR}/examples/${EXAMPLE}" "$@"
        ;;
    format)
        bash scripts/format-code.sh
        ;;
    format-check)
        bash scripts/format-code.sh --check
        ;;
    forbidden)
        bash scripts/check-forbidden-usage.sh
        ;;
    tidy)
        command -v run-clang-tidy >/dev/null 2>&1 || die "run-clang-tidy not found: use 'make docker-tidy'"
        # Analyse a Clang build so compile_commands.json has Clang flags.
        [[ "${COMPILER}" == clang* ]] || COMPILER=clang
        configure tidy Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF
        build
        files=()
        while IFS= read -r f; do files+=("${f}"); done < <(find tests examples -name '*.cpp' 2>/dev/null | sort)
        [[ ${#files[@]} -gt 0 ]] || die "no translation units to analyse"
        run-clang-tidy -p "${BUILD_DIR}" -quiet -j "${JOBS}" \
            -header-filter='.*/include/stepwright/.*\.hpp$' "${files[@]}"
        ;;
    cppcheck)
        configure cppcheck Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF
        bash scripts/run-cppcheck.sh -b "${BUILD_DIR}"
        ;;
    coverage)
        # gcov must match the compiler that produced the .gcno files.
        if [[ -n "${STEPWRIGHT_IN_CONTAINER:-}" ]]; then export CXX=g++; fi
        STEPWRIGHT_BUILD_ROOT="${BUILD_ROOT}" python3 scripts/coverage.py --jobs "${JOBS}" "$@"
        ;;
    check)
        "$0" forbidden
        BUILD_TYPE=Release "$0" test
        "$0" debug
        "$0" asan
        "$0" tsan
        ;;
    *)
        die "unknown task '${TASK}'"
        ;;
esac
