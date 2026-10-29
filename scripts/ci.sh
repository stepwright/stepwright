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
#   format | format-check | tidy | cppcheck | forbidden | coverage | docs | consumer | check
#
# Environment:
#   COMPILER    native | gcc | clang | clang-libcxx | gcc13 | clang18
#   BUILD_TYPE  Release | Debug | RelWithDebInfo      (build/test/bench; default Release)
#   BUILD_ROOT  where build trees live                (default: <repo>/build)
#   JOBS        parallelism                           (default: CPU count)
#   EXAMPLE     example to run for `run`
#   BENCH_ARGS  extra Google Benchmark arguments
#   BENCH_OUT   bench: also write <dir>/bench_*.json (relative to the repo root)
#   BENCH_LABEL bench: platform label stored in the JSON (default: uname)
#   CMAKE_ARGS  extra CMake configure arguments
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

TASK="${1:-}"
[[ -n "${TASK}" ]] || { sed -n '2,27p' "$0"; exit 2; }
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
BENCH_ARGS="${BENCH_ARGS:---benchmark_min_time=1s --benchmark_repetitions=10 --benchmark_report_aggregates_only=true}"

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
        gcc13) COMPILER_ARGS+=("-DCMAKE_CXX_COMPILER=g++-13") ;;
        clang18) COMPILER_ARGS+=("-DCMAKE_CXX_COMPILER=clang++-18") ;;
        *) die "unknown COMPILER='${COMPILER}' (native|gcc|clang|clang-libcxx|gcc13|clang18)" ;;
    esac
}

source "${REPO_ROOT}/scripts/cmake_generator.sh"

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
    stepwright_select_generator "${BUILD_DIR}"
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
        if [[ -n "${BENCH_OUT:-}" ]]; then
            # JSON per benchmark executable, plus the context bench_report.py prints.
            [[ "${BENCH_OUT}" = /* ]] || BENCH_OUT="${REPO_ROOT}/${BENCH_OUT}"
            mkdir -p "${BENCH_OUT}"
            cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "${BUILD_DIR}/CMakeCache.txt")"
            # The code-generation flags the benchmarks were actually compiled with.
            flags="$(python3 - "${BUILD_DIR}/compile_commands.json" <<'PY' || true
import json, shlex, sys
for entry in json.load(open(sys.argv[1])):
    if entry["file"].endswith("benchmarks/bench_engine.cpp"):
        args = entry.get("arguments") or shlex.split(entry["command"])
        keep = [a for a in args[1:] if a.startswith(("-O", "-D", "-std", "-f", "-m", "-stdlib", "-g"))
                and not a.startswith(("-fdiagnostics", "-ferror-limit", "-fcolor", "-DBENCHMARK_"))]
        print(" ".join(keep))
        break
PY
)"
            if [[ "${OS}" == "Darwin" ]]; then
                cpu="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
            else
                cpu="$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo 2>/dev/null | head -1)"
                [[ -n "${cpu}" ]] || cpu="$(uname -m)"
            fi
            [[ -z "${BENCH_HOST_CPU:-}" ]] || cpu="${BENCH_HOST_CPU}"
            commit="${GIT_COMMIT:-$(git -C "${REPO_ROOT}" describe --always --dirty 2>/dev/null || echo unknown)}"
            # Google Benchmark's --benchmark_context cannot carry '=' or ',' in values:
            # write the build context next to the results instead.
            COMPILER_VERSION="$("${cxx}" --version | head -1)" FLAGS="${flags}" CPU="${cpu}" \
                COMMIT="${commit}" LABEL="${BENCH_LABEL:-$(uname -s) $(uname -m)}" \
                OS_NAME="$(uname -srm)" ARGS="${BENCH_ARGS}" python3 - "${BENCH_OUT}/context.json" <<'PY'
import hashlib, json, os, pathlib, sys
keys = {"compiler": "COMPILER_VERSION", "flags": "FLAGS", "cpu": "CPU", "commit": "COMMIT",
        "label": "LABEL", "os": "OS_NAME", "bench_args": "ARGS"}
source = hashlib.sha256()
paths = sorted(pathlib.Path("include/stepwright").rglob("*.hpp")) + sorted(pathlib.Path("benchmarks").glob("*.cpp"))
for path in paths:
    source.update(str(path).encode() + b"\0" + path.read_bytes())
context = {k: os.environ[v] for k, v in keys.items()}
context["source_sha256"] = source.hexdigest()
context["host_load_avg"] = os.environ.get("BENCH_HOST_LOAD") or " / ".join(f"{x:.2f}" for x in os.getloadavg())
with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(context, handle, indent=1)
PY
        fi
        found=0
        for b in "${BUILD_DIR}"/benchmarks/bench_*; do
            [[ -x "${b}" && -f "${b}" ]] || continue
            found=1
            log "$(basename "${b}")"
            extra=()
            if [[ -n "${BENCH_OUT:-}" ]]; then
                extra+=("--benchmark_out=${BENCH_OUT}/$(basename "${b}").json"
                        "--benchmark_out_format=json")
            fi
            # shellcheck disable=SC2086
            "${b}" ${BENCH_ARGS} ${extra[@]+"${extra[@]}"} "$@"
            if [[ -n "${BENCH_OUT:-}" ]]; then
                # Published results must not reveal local paths (user names,
                # directory layout): keep only the executable's file name.
                python3 - "${BENCH_OUT}/$(basename "${b}").json" <<'PY'
import re
import sys

path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    text = f.read()
text = re.sub(r'("executable": ")[^"]*/', r"\1", text)
with open(path, "w", encoding="utf-8") as f:
    f.write(text)
PY
            fi
        done
        [[ ${found} -eq 1 ]] || log "no benchmarks built yet"
        ;;
    run)
        [[ -n "${EXAMPLE:-}" ]] || die "set EXAMPLE=<name>"
        configure "$(lower "${BUILD_TYPE}")" "${BUILD_TYPE}"
        build --target "${EXAMPLE}"
        "${BUILD_DIR}/examples/${EXAMPLE}" "$@"
        ;;
    docs)
        [[ -n "${STEPWRIGHT_IN_CONTAINER:-}" ]] || die "docs use the dev container: make docs"
        command -v doxygen >/dev/null 2>&1 || die "Doxygen is missing from the dev image"
        mkdir -p "${BUILD_ROOT}/docs"
        (cat Doxyfile; printf '\nOUTPUT_DIRECTORY = "%s"\nUSE_MDFILE_AS_MAINPAGE = "%s/README.md"\n' \
            "${BUILD_ROOT}/docs" "${REPO_ROOT}") | doxygen -
        ;;
    consumer)
        configure consumer Release -DSTEPWRIGHT_BUILD_TESTS=OFF -DSTEPWRIGHT_BUILD_EXAMPLES=OFF \
            -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF
        build
        # Move the install to test relocation; no build-tree include paths may leak.
        prefix="${BUILD_DIR}/prefix"
        relocated="${BUILD_DIR}/relocated prefix"
        cmake --install "${BUILD_DIR}" --prefix "${prefix}"
        # Repeated runs replace only our generated install tree, never user files.
        if [[ -d "${relocated}" ]]; then
            cmake -E rm -rf "${relocated}"
        fi
        cmake -E rename "${prefix}" "${relocated}"
        for mode in install fetch subdirectory; do
            log "consumer ${mode} (${COMPILER})"
            stepwright_select_generator "${BUILD_DIR}/${mode}"
            cmake -S tests/consumer -B "${BUILD_DIR}/${mode}" \
                ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
                ${COMPILER_ARGS[@]+"${COMPILER_ARGS[@]}"} \
                -DCMAKE_BUILD_TYPE=Release -DSTEPWRIGHT_CONSUMER_MODE="${mode}" \
                -DSTEPWRIGHT_SOURCE="${REPO_ROOT}" -DCMAKE_PREFIX_PATH="${relocated}"
            cmake --build "${BUILD_DIR}/${mode}" -j "${JOBS}"
            ctest --test-dir "${BUILD_DIR}/${mode}" --output-on-failure
        done
        # pkg-config is required in the container; native machines may lack it.
        if command -v pkg-config >/dev/null 2>&1; then
            pc_dir="$(dirname "$(find "${relocated}" -name stepwright.pc -print -quit)")"
            export PKG_CONFIG_PATH="${pc_dir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
            cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "${BUILD_DIR}/CMakeCache.txt")"
            # Respect pkg-config's escaping (the relocated prefix contains spaces)
            # and the selected standard library, without evaluating shell output.
            python3 - "${cxx}" "${BUILD_DIR}" <<'PY'
import pathlib, shlex, subprocess, sys
cxx, build = sys.argv[1:]
cache = pathlib.Path(build, "CMakeCache.txt").read_text()
flags = next((line.split("=", 1)[1] for line in cache.splitlines()
              if line.startswith("CMAKE_CXX_FLAGS:STRING=")), "")
pc = subprocess.check_output(["pkg-config", "--cflags", "--libs", "stepwright"], text=True)
subprocess.run([cxx, *shlex.split(flags), "tests/consumer/main.cpp", "tests/consumer/peer.cpp",
                *shlex.split(pc), "-o", str(pathlib.Path(build, "pkg-consumer"))], check=True)
PY
            "${BUILD_DIR}/pkg-consumer"
        elif [[ -n "${STEPWRIGHT_IN_CONTAINER:-}" ]]; then
            die "pkg-config is missing from the dev image"
        else
            log "pkg-config unavailable natively; .pc is exercised in docker-consumer"
        fi
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
        while IFS= read -r f; do files+=("${f}"); done < <(find tests examples -name '*.cpp' ! -path 'tests/consumer/*' 2>/dev/null | sort)
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
