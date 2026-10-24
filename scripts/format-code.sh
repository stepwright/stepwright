#!/usr/bin/env bash
# Format (or check) stepwright sources with clang-format.
#
# Walks stepwright's source tree (include/, src/, tests/, examples/, benchmarks/)
# and applies the rules in .clang-format. Skips _deps/ and build*/.
#
# Usage:
#   scripts/format-code.sh            # rewrite files in-place
#   scripts/format-code.sh --check    # dry-run, exit non-zero on diffs (CI mode)

set -eo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
MODE="apply"

case "${1:-}" in
    "")        ;;
    --check)   MODE="check" ;;
    -h|--help)
        sed -n '2,12p' "$0" | sed 's|^# \{0,1\}||'
        exit 0
        ;;
    *)
        echo "Unknown arg: $1" >&2
        exit 2
        ;;
esac

if ! command -v "${CLANG_FORMAT}" >/dev/null 2>&1; then
    echo "error: ${CLANG_FORMAT} not found on PATH (set \$CLANG_FORMAT to override)." >&2
    exit 1
fi

cd "${REPO_ROOT}"

# Collect files. Skip build/, _deps/, any sibling build-* trees.
# Use NUL-delimited xargs instead of bash 4's `mapfile`, since macOS still
# ships bash 3.2 by default.
SRC_DIRS=()
for d in include src tests examples benchmarks; do
    [[ -d "${d}" ]] && SRC_DIRS+=("${d}")
done
if [[ ${#SRC_DIRS[@]} -eq 0 ]]; then
    echo "No source directories found." >&2
    exit 1
fi

FIND_CMD=(find "${SRC_DIRS[@]}"
    \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \)
    -not -path '*/_deps/*'
    -not -path '*/build/*'
    -not -path '*/build-*/*'
    -print0)

FILE_COUNT=$("${FIND_CMD[@]}" | tr -cd '\0' | wc -c | tr -d ' ')
if [[ "${FILE_COUNT}" -eq 0 ]]; then
    echo "No source files found under include/ src/ tests/ examples/ benchmarks/." >&2
    exit 1
fi

echo ">>> ${MODE} formatting ${FILE_COUNT} files with $(${CLANG_FORMAT} --version | head -n1)"

if [[ "${MODE}" == "check" ]]; then
    if "${FIND_CMD[@]}" | xargs -0 "${CLANG_FORMAT}" -style=file -fallback-style=none --dry-run --Werror; then
        echo "Format check OK."
    else
        echo "error: format diffs above; run 'make format' to fix." >&2
        exit 1
    fi
else
    "${FIND_CMD[@]}" | xargs -0 "${CLANG_FORMAT}" -style=file -fallback-style=none -i
    echo "Formatting applied."
fi
