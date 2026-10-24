#!/usr/bin/env bash
# Install stepwright's developer toolchain (compilers + static-analysis + coverage).
#
# Designed to be a single-command bootstrap for new contributors and a one-line
# replacement for the inline `apt-get install` blocks in our GitHub Actions
# workflows.
#
# Usage:
#   scripts/install_toolchain.sh                # install everything (== "all")
#   scripts/install_toolchain.sh build          # only a C++20 compiler + cmake + ninja
#   scripts/install_toolchain.sh format tidy    # multiple groups
#
# Groups:
#   build        cmake, ninja-build, gcc, g++, python3
#   format       clang-format-22
#   tidy         clang-tidy-22 + clang-22
#   cppcheck     cppcheck
#   coverage     lcov + gcc/g++ + python3 (for scripts/coverage.py)
#   sanitizers   clang-22 (so ASan/TSan/UBSan/LSan builds work end-to-end)
#   all          every group above
#
# Notes:
#   * Linux  -> uses apt-get; expects Ubuntu / Debian. Uses clang-format-22,
#               clang-tidy-22, and clang-22 via LLVM apt repository.
#   * macOS  -> uses Homebrew. Versions are whatever brew currently ships.
#   * Idempotent. Re-running is safe.
#   * Skips `sudo` when already root or when running on macOS.
#   * Set `DRY_RUN=1` to print the install commands instead of executing them.

# NOTE on `set -u`: this script must run on macOS's stock bash 3.2 *and* on
# Ubuntu's bash 5.x. Bash 3.2 has known bugs around `${arr[@]}` expansion under
# `set -u` (see https://lists.gnu.org/archive/html/bug-bash/2010-06/msg00098.html),
# so we deliberately stick with `set -eo pipefail` only.
set -eo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/install_toolchain.sh [GROUPS...]

Groups: build | format | tidy | cppcheck | coverage | sanitizers | all
Default (no args) is "all".

Examples:
  scripts/install_toolchain.sh
  scripts/install_toolchain.sh build format
  scripts/install_toolchain.sh tidy
EOF
}

# Parse args.
#
# NOTE: do NOT name this array `GROUPS` -- bash defines `GROUPS` as a
# read-only built-in array of the caller's group IDs, which silently swallows
# assignment and breaks the rest of the script in confusing ways.
if [[ $# -eq 0 ]]; then
    INSTALL_GROUPS=(all)
else
    INSTALL_GROUPS=("$@")
fi

for g in "${INSTALL_GROUPS[@]}"; do
    case "${g}" in
        -h|--help) usage; exit 0 ;;
        all|build|format|tidy|cppcheck|coverage|sanitizers) ;;
        *)
            echo "error: unknown group '${g}'" >&2
            usage >&2
            exit 2
            ;;
    esac
done

# Expand "all" so the per-OS install paths don't have to special-case it.
if printf '%s\n' "${INSTALL_GROUPS[@]}" | grep -qx all; then
    INSTALL_GROUPS=(build format tidy cppcheck coverage sanitizers)
fi

want() {
    local target="$1"
    printf '%s\n' "${INSTALL_GROUPS[@]}" | grep -qx "${target}"
}

# sudo wrapper: no-op when we're already root or running on macOS (brew refuses
# to be invoked under sudo).
SUDO=""
if [[ "$(uname -s)" != "Darwin" && "${EUID:-$(id -u)}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    fi
fi

# DRY_RUN=1 turns every package-manager invocation into an `echo` so users can
# preview what the script will do without touching the system.
RUN="eval"
if [[ "${DRY_RUN:-0}" = "1" ]]; then
    RUN="echo DRY_RUN:"
fi

install_linux() {
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "error: this script currently supports apt-based Linux only " \
             "(Ubuntu / Debian). Found: $(uname -a)" >&2
        exit 1
    fi

    # Add LLVM repository for newer clang-format versions
    if want format || want tidy || want sanitizers; then
        echo ":: Adding LLVM apt repository for clang-22"
        ${RUN} "${SUDO} apt-get update -y"
        ${RUN} "${SUDO} apt-get install -y --no-install-recommends wget gnupg software-properties-common"
        ${RUN} "wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | ${SUDO} tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc"
        ${RUN} "${SUDO} add-apt-repository -y 'deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-22 main'"
        ${RUN} "${SUDO} apt-get update -y"
    fi

    local pkgs=()

    want build      && pkgs+=(cmake ninja-build gcc g++ python3 git)
    want format     && pkgs+=(clang-format-22)
    want tidy       && pkgs+=(clang-tidy-22 clang-22)
    want cppcheck   && pkgs+=(cppcheck)
    want coverage   && pkgs+=(lcov gcc g++ python3)
    want sanitizers && pkgs+=(clang-22)

    # De-dupe (avoid bash 4's `mapfile`; this script also runs on macOS
    # bash 3.2 even though the apt path itself is Linux-only).
    local uniq_pkgs_str
    uniq_pkgs_str=$(printf '%s\n' "${pkgs[@]}" | awk '!seen[$0]++' | tr '\n' ' ')

    echo ":: apt-get install: ${uniq_pkgs_str}"
    ${RUN} "${SUDO} apt-get install -y --no-install-recommends ${uniq_pkgs_str}"

    # Pin clang-format / clang-tidy to version 22, then set up alternatives
    # so plain `clang-format` and `clang-tidy` resolve correctly.
    if want format; then
        ${RUN} "${SUDO} update-alternatives --install \
            /usr/bin/clang-format clang-format \
            /usr/bin/clang-format-22 100"
    fi

    if want tidy; then
        ${RUN} "${SUDO} update-alternatives --install \
            /usr/bin/clang-tidy clang-tidy \
            /usr/bin/clang-tidy-22 100"
        # `run-clang-tidy` ships under the versioned name on Ubuntu.
        if [[ -x "/usr/bin/run-clang-tidy-22" ]] || [[ "${DRY_RUN:-0}" = "1" ]]; then
            ${RUN} "${SUDO} update-alternatives --install \
                /usr/bin/run-clang-tidy run-clang-tidy \
                /usr/bin/run-clang-tidy-22 100"
        fi
    fi
}

install_macos() {
    if ! command -v brew >/dev/null 2>&1; then
        echo "error: Homebrew not found. Install it first:" >&2
        echo "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"" >&2
        exit 1
    fi

    local formulae=()

    want build      && formulae+=(cmake ninja python@3)
    want format     && formulae+=(clang-format)
    want tidy       && formulae+=(llvm)
    want cppcheck   && formulae+=(cppcheck)
    want coverage   && formulae+=(lcov python@3)
    want sanitizers && formulae+=(llvm)

    # De-dupe.
    local uniq_formulae
    uniq_formulae=$(printf '%s\n' "${formulae[@]}" | awk '!seen[$0]++')

    echo ":: brew install: ${uniq_formulae//$'\n'/ }"
    # `brew install` exits non-zero if a formula is already up-to-date in some
    # versions; tolerate that.
    ${RUN} "brew install ${uniq_formulae//$'\n'/ } || true"

    if want tidy || want sanitizers; then
        echo
        echo ":: NOTE: Homebrew's llvm/clang-tidy is not on PATH by default."
        echo "         Add this to your shell rc to use it:"
        echo "             export PATH=\"\$(brew --prefix llvm)/bin:\$PATH\""
    fi
}

case "$(uname -s)" in
    Linux)  install_linux ;;
    Darwin) install_macos ;;
    *)
        echo "error: unsupported OS: $(uname -s). " \
             "Only Linux (apt) and macOS (brew) are supported." >&2
        exit 1
        ;;
esac

echo
echo ":: stepwright toolchain installed: ${INSTALL_GROUPS[*]}"
