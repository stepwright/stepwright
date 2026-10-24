#!/usr/bin/env bash
# Run cppcheck against stepwright sources.
#
# Notes:
#   * No protobuf / codegen pre-build step -- stepwright has no generated code.
#   * Job-count detection works on both macOS and Linux.
#   * Cppcheck version is not pinned; anything >= 2.x is fine.
#
# Usage:
#   scripts/run-cppcheck.sh [-b BUILD_DIR] [-t TARGET] [-s] [-e "EXTRA"]
#
# The script expects a CMake build dir with compile_commands.json so that
# cppcheck can analyse the exact compile command for each translation unit.

set -eo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/default"
TARGET=""
QUIET="--quiet"
EXTRA_OPTS=""

usage() {
    cat <<EOF
usage: $(basename "$0") [options]

  -b, --build-dir DIR    CMake build directory (default: build/default).
  -t, --target PATH      Restrict scan to a path or glob (default: whole project
                         via compile_commands.json).
  -s, --show-progress    Show cppcheck progress (disables --quiet).
  -e, --extra "OPTS"     Extra options forwarded verbatim to cppcheck.
  -h, --help             Show this help.

Environment:
  CPPCHECK    Override the cppcheck executable.
  JOBS        Override parallelism (default: hw.ncpu / nproc).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-dir)   BUILD_DIR="$2"; shift 2 ;;
        -t|--target)      TARGET="$2"; shift 2 ;;
        -s|--show-progress) QUIET=""; shift ;;
        -e|--extra)       EXTRA_OPTS="$2"; shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *)                echo "Unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

CPPCHECK="${CPPCHECK:-cppcheck}"
if ! command -v "${CPPCHECK}" >/dev/null 2>&1; then
    echo "error: cppcheck not found on PATH (set \$CPPCHECK to override)." >&2
    exit 1
fi

if [[ -z "${JOBS:-}" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else
        JOBS=4
    fi
fi

CPPCHECK_BUILD_DIR="${BUILD_DIR}/cppcheck"
REPORTS_DIR="${CPPCHECK_BUILD_DIR}/reports"
SUPPRESSIONS="${REPO_ROOT}/.cppcheck/suppressions.txt"

mkdir -p "${REPORTS_DIR}"

LOAD_PROJECT_OPT=()
if [[ -z "${TARGET}" ]]; then
    if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
        echo "error: ${BUILD_DIR}/compile_commands.json missing -- run 'make configure' first." >&2
        exit 1
    fi
    LOAD_PROJECT_OPT=(--project="${BUILD_DIR}/compile_commands.json")
    TARGET_ARG=()
else
    TARGET_ARG=("${TARGET}")
fi

REPORT="${REPORTS_DIR}/full_report.txt"
rm -f "${REPORT}"

echo ">>> Running cppcheck (jobs=${JOBS}, build-dir=${CPPCHECK_BUILD_DIR})"
set -x
"${CPPCHECK}" \
    "${LOAD_PROJECT_OPT[@]}" \
    --enable=warning,style,performance,portability \
    --inconclusive \
    --inline-suppr \
    --std=c++20 \
    --library=googletest \
    --platform=unix64 \
    --max-configs=1 \
    --error-exitcode=0 \
    -j "${JOBS}" \
    --cppcheck-build-dir="${CPPCHECK_BUILD_DIR}" \
    --suppressions-list="${SUPPRESSIONS}" \
    --output-file="${REPORT}" \
    -i"${BUILD_DIR}" -iCMakeFiles \
    ${QUIET} \
    ${EXTRA_OPTS} \
    "${TARGET_ARG[@]}" || true
set +x

echo
echo "========== Cppcheck results by category =========="
categories=("(error)" "(warning)" "(style)" "(performance)" "(portability)" "(information)" "(unusedFunction)")
found=false
for cat in "${categories[@]}"; do
    if grep -q -- "${cat}" "${REPORT}" 2>/dev/null; then
        out="${REPORTS_DIR}/$(echo "${cat}" | tr -d '()').txt"
        grep -- "${cat}" "${REPORT}" > "${out}"
        printf "  %-15s %s\n" "${cat}" "$(wc -l < "${out}" | tr -d ' ') lines -> ${out}"
        found=true
    fi
done
$found || echo "  (no issues found)"
echo "=================================================="

if grep -qE "\(error\)|\(warning\)" "${REPORT}" 2>/dev/null; then
    echo "Cppcheck found errors or warnings (see ${REPORT})." >&2
    exit 1
fi

echo "Cppcheck OK. Full report: ${REPORT}"
