#!/usr/bin/env bash
# Scan stepwright sources for patterns that should never appear in shipped
# library code (under include/ and src/).
#
# Tests, benchmarks, and examples are intentionally exempt -- they are free
# to use cout/printf/assert.
#
# Patterns enforced:
#
#   1. Bare NOLINT / NOLINTNEXTLINE without any justification.
#      Accepted forms:
#         // NOLINT(check-name)
#         // NOLINT: <reason>
#         // NOLINTNEXTLINE: <reason>
#      Rejected: a lone "// NOLINT" with no qualifier.
#
#   2. std::cout, std::cerr, or stand-alone printf in headers / src.
#      Library output should go through STEPWRIGHT_LOG_* macros (stepwright/detail/log.hpp)
#      so it can be compiled out in release builds.
#
# Notes:
#   * C-style assert() is intentionally NOT forbidden here. stepwright uses
#     assert() as an invariant-checking mechanism for internal preconditions
#     (see include/stepwright/). Asserts
#     compile out under -DNDEBUG, which is the desired behaviour.
#
# Exits 0 when clean, 1 when any forbidden pattern is found.

set -eo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

SEARCH_PATHS=(include src)
GREP_OPTS=(
    -rnE
    --color=auto
    --include='*.h'
    --include='*.hpp'
    --include='*.cpp'
    --exclude-dir=build
    --exclude-dir=build-coverage
    --exclude-dir=build-trace
    --exclude-dir=_deps
)

violations=0

check_pattern() {
    local label="$1" pattern="$2"
    if grep "${GREP_OPTS[@]}" -- "${pattern}" "${SEARCH_PATHS[@]}" 2>/dev/null; then
        echo "error: forbidden pattern (${label}) -- see matches above." >&2
        violations=$((violations + 1))
    fi
}

# 1. NOLINT without a parenthesised check name AND without a colon-prefixed
#    reason. Accepts: "NOLINT(...)", "NOLINT: text", "NOLINTNEXTLINE: text".
check_pattern "unjustified NOLINT" 'NOLINT(NEXTLINE)?[[:space:]]*(//|$)'

# 2. Stray std::cout / std::cerr / printf in shipped library code.
check_pattern "std::cout / std::cerr / printf" '(std::c(out|err)|(^|[^_[:alnum:]])printf)\b'

if [[ ${violations} -gt 0 ]]; then
    echo "check-forbidden-usage: ${violations} pattern(s) violated." >&2
    exit 1
fi

echo "check-forbidden-usage: OK"
