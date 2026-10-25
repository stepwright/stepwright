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
#   3. Internal planning links in every tracked text file. A NUL-delimited
#      STEPWRIGHT_TRACKED_FILES manifest supports containerized linked worktrees.
#      Git/manifest failures are errors, never evidence of a clean scan.
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

# Public files must not link to internal planning directories. A linked worktree's
# Git metadata may live outside the container mount: make supplies its host-side
# tracked-file list in that case. Never mistake a Git/manifest error for a clean
# scan. Read working-tree contents, including intent-to-add files, not the index.
if ! python3 - "${STEPWRIGHT_TRACKED_FILES:-}" <<'PY'
import os
import pathlib
import subprocess
import sys

try:
    if sys.argv[1]:
        tracked = pathlib.Path(sys.argv[1]).read_bytes()
    else:
        tracked = subprocess.check_output(["git", "ls-files", "-z"])
    if not tracked or not tracked.endswith(b"\0"):
        raise ValueError("tracked-file list is empty or not NUL-delimited")
    needle = b"." + b"claude/"
    found = False
    for raw_name in tracked.split(b"\0")[:-1]:
        path = pathlib.Path(os.fsdecode(raw_name))
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("tracked-file list contains a path outside the repository")
        try:
            if path.is_symlink():
                data = os.fsencode(os.readlink(path))  # never follow links outside the repo
            elif path.is_dir():
                continue  # a submodule entry, not a tracked text file
            else:
                data = path.read_bytes()
        except FileNotFoundError:
            continue  # a tracked deletion in the working tree
        if b"\0" in data:
            continue  # binary, like git grep -I
        for line, text in enumerate(data.splitlines(), 1):
            if needle in text:
                print(f"{path}:{line}: {text.decode('utf-8', errors='replace')}")
                found = True
    if found:
        sys.exit("error: internal planning reference in a tracked file")
except (OSError, ValueError, subprocess.CalledProcessError) as error:
    sys.exit(f"error: unable to scan tracked files: {error}")
PY
then
    violations=$((violations + 1))
fi

if [[ ${violations} -gt 0 ]]; then
    echo "check-forbidden-usage: ${violations} pattern(s) violated." >&2
    exit 1
fi

echo "check-forbidden-usage: OK"
