#!/usr/bin/env python3
"""End-to-end code coverage pipeline for stepwright.

Handles tool installation, configures a dedicated coverage build dir, runs
the test suite, captures gcov data, filters out system / third-party paths,
and emits an HTML report at ``build/coverage/coverage_html/index.html``.

The script is platform aware:

  * macOS (AppleClang): wraps ``xcrun llvm-cov gcov`` so ``lcov`` can decode
    the ``.gcno``/``.gcda`` files. Without this shim ``lcov`` silently
    drops every project source file and the report ends up containing only
    SDK / libc++ headers.
  * Linux (gcc/clang): uses the system ``gcov`` directly.

Run ``python3 scripts/coverage.py --help`` for the full CLI surface.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import webbrowser
from pathlib import Path
from typing import Iterable, List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = Path(os.environ.get("STEPWRIGHT_BUILD_ROOT", str(REPO_ROOT / "build"))) / "coverage"
RAW_INFO = BUILD_DIR / "coverage.raw.info"
INFO = BUILD_DIR / "coverage.info"
HTML_DIR = BUILD_DIR / "coverage_html"
GCOV_SHIM = BUILD_DIR / "llvm-gcov.sh"

# Patterns stripped from the captured info file. lcov uses shell-style
# globbing here, so make the patterns broad enough to catch macOS SDK
# paths under /Applications/Xcode.app/... and libc++ under */v1/*.
REMOVE_PATTERNS = [
    "/usr/*",
    "/Library/*",
    "/Applications/*",
    "*/v1/*",
    "*/_deps/*",
    "*/tests/*",
    "*/benchmarks/*",
    "*/examples/*",
    "*/build/*",
    "*/build-*/*",
]

# After --remove we --extract to restrict to stepwright-owned source. This
# catches cases where a header was indirectly captured from somewhere
# unexpected (for example a sibling checkout outside the repo).
EXTRACT_PATTERNS = [
    str(REPO_ROOT / "src" / "*"),
    str(REPO_ROOT / "include" / "*"),
]

# Errors that lcov raises by default but that are routine on macOS
# (mismatched line counts between gcno/gcda from inlined templates,
# unused --remove patterns, etc.). Downgrading them keeps the run green
# without hiding real problems: lcov still prints warnings.
#
# NOTE: Compatible with lcov 1.14+ (Ubuntu 20.04/22.04/24.04).
# Excludes lcov 2.x-only error types (range, empty, utility, unsupported, category)
# to maintain compatibility with GitHub Actions ubuntu-latest runners.
LCOV_IGNORE = "inconsistent,format,unused,source,mismatch,negative,corrupt,empty"


class CoverageError(RuntimeError):
    """Raised when the pipeline cannot continue."""


def info(msg: str) -> None:
    print(f"\033[36m[coverage]\033[0m {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"\033[33m[coverage] warn:\033[0m {msg}", flush=True)


def fail(msg: str) -> "None":
    print(f"\033[31m[coverage] error:\033[0m {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd: List[str], *, cwd: Optional[Path] = None, check: bool = True) -> subprocess.CompletedProcess:
    """Run ``cmd`` and stream its output. Raises on non-zero exit by default."""
    info("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=check)


def which(tool: str) -> Optional[str]:
    return shutil.which(tool)


def detect_platform() -> str:
    return platform.system()


def cpu_count() -> int:
    try:
        return os.cpu_count() or 4
    except Exception:
        return 4


# --------------------------------------------------------------------------
# Tool installation
# --------------------------------------------------------------------------


def ensure_lcov(auto_install: bool) -> None:
    """Make sure ``lcov`` and ``genhtml`` are on PATH, installing if needed."""
    missing = [t for t in ("lcov", "genhtml") if which(t) is None]
    if not missing:
        return

    system = detect_platform()
    info(f"missing tools: {', '.join(missing)}")

    if not auto_install:
        fail(
            "lcov / genhtml not found and --no-install was passed. Install manually:\n"
            "  macOS:        brew install lcov\n"
            "  Debian/Ubuntu sudo apt-get install -y lcov\n"
            "  Fedora:       sudo dnf install -y lcov\n"
            "  Arch:         sudo pacman -S --noconfirm lcov"
        )

    if system == "Darwin":
        if which("brew") is None:
            fail("Homebrew not found. Install brew (https://brew.sh) or pass --no-install and install lcov yourself.")
        run(["brew", "install", "lcov"])
    elif system == "Linux":
        if which("apt-get"):
            run(["sudo", "apt-get", "update"])
            run(["sudo", "apt-get", "install", "-y", "lcov"])
        elif which("dnf"):
            run(["sudo", "dnf", "install", "-y", "lcov"])
        elif which("pacman"):
            run(["sudo", "pacman", "-S", "--noconfirm", "lcov"])
        else:
            fail("No supported package manager (apt-get/dnf/pacman) found. Install lcov manually.")
    else:
        fail(f"Unsupported platform: {system}. Install lcov manually and re-run with --no-install.")

    if any(which(t) is None for t in ("lcov", "genhtml")):
        fail("lcov installation finished but the tools are still not on PATH.")


def ensure_cmake_ctest() -> None:
    for tool in ("cmake", "ctest"):
        if which(tool) is None:
            fail(f"{tool} is required but not on PATH.")


# --------------------------------------------------------------------------
# gcov shim for AppleClang
# --------------------------------------------------------------------------


def write_llvm_gcov_shim() -> Path:
    """Create a tiny wrapper so lcov can use ``xcrun llvm-cov gcov``."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    GCOV_SHIM.write_text(
        "#!/usr/bin/env bash\n"
        "# Auto-generated by scripts/coverage.py - do not edit.\n"
        'exec xcrun llvm-cov gcov "$@"\n'
    )
    GCOV_SHIM.chmod(0o755)
    return GCOV_SHIM


def write_llvm_gcov_shim_linux() -> Path:
    """Linux variant: invoke ``llvm-cov gcov`` directly (no xcrun)."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    GCOV_SHIM.write_text(
        "#!/usr/bin/env bash\n"
        "# Auto-generated by scripts/coverage.py - do not edit.\n"
        'exec llvm-cov gcov "$@"\n'
    )
    GCOV_SHIM.chmod(0o755)
    return GCOV_SHIM


def detect_compiler_id() -> Optional[str]:
    """Return ``CMAKE_CXX_COMPILER_ID`` from the configured build dir.

    Returns ``None`` if the cache is missing or the entry is absent
    (e.g. the script was invoked with ``--skip-tests`` against a stale
    or pre-existing build directory).
    """
    cache = BUILD_DIR / "CMakeCache.txt"
    if not cache.exists():
        return None
    try:
        for line in cache.read_text(errors="replace").splitlines():
            # Lines look like:  CMAKE_CXX_COMPILER_ID:STRING=Clang
            if line.startswith("CMAKE_CXX_COMPILER_ID:"):
                _, _, value = line.partition("=")
                return value.strip() or None
    except OSError:
        return None
    return None


def resolve_gcov_tool() -> str:
    """Return the ``--gcov-tool`` value lcov should use on this host.

    Decision matrix:

    =====================  ==================================
    OS / compiler          gcov tool
    =====================  ==================================
    macOS / AppleClang     ``xcrun llvm-cov gcov`` (shim)
    Linux  / Clang         ``llvm-cov gcov`` (shim)
    Linux  / GNU           system ``gcov``
    Other  / GNU           system ``gcov``
    =====================  ==================================

    Without the shim, ``lcov`` pipes ``.gcno``/``.gcda`` files emitted
    by clang into a gcc-style ``gcov`` binary that cannot decode them
    and silently drops every project source file from the report.
    """
    system = detect_platform()
    compiler = detect_compiler_id()

    if system == "Darwin":
        if which("xcrun") is None:
            fail("xcrun not found - is the Xcode Command Line Tools package installed? Try: xcode-select --install")
        try:
            subprocess.run(
                ["xcrun", "llvm-cov", "gcov", "--version"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except (FileNotFoundError, subprocess.CalledProcessError):
            fail("xcrun llvm-cov gcov failed. Run `xcode-select --install` and retry.")
        info("using xcrun llvm-cov gcov shim (macOS / AppleClang)")
        return str(write_llvm_gcov_shim())

    # Non-Darwin (Linux, *BSD, ...): pick based on compiler family.
    if compiler in {"Clang", "AppleClang"}:
        if which("llvm-cov") is None:
            fail(
                "Compiler is Clang but llvm-cov is not on PATH. "
                "Install LLVM (e.g. `apt-get install llvm` / `dnf install llvm`) "
                "or rebuild with gcc."
            )
        info(f"using llvm-cov gcov shim (compiler: {compiler})")
        return str(write_llvm_gcov_shim_linux())

    if which("gcov") is None:
        if which("llvm-cov") is not None:
            warn("gcov not found; falling back to llvm-cov gcov shim")
            return str(write_llvm_gcov_shim_linux())
        fail("Neither gcov nor llvm-cov found on PATH.")

    if compiler is None:
        info("using system gcov (compiler family unknown - run --clean if results look empty)")
    else:
        info(f"using system gcov (compiler: {compiler})")
    return "gcov"


# --------------------------------------------------------------------------
# Build pipeline
# --------------------------------------------------------------------------


def clean_build_dir() -> None:
    if BUILD_DIR.exists():
        info(f"removing {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)


def wipe_gcda() -> None:
    """Remove stale .gcda files so re-runs reflect the latest test execution."""
    if not BUILD_DIR.exists():
        return
    removed = 0
    for path in BUILD_DIR.rglob("*.gcda"):
        try:
            path.unlink()
            removed += 1
        except OSError:
            pass
    if removed:
        info(f"removed {removed} stale .gcda files")


def configure(extra_cmake_args: Iterable[str]) -> None:
    info("configuring CMake build with coverage instrumentation")
    cmd = [
        "cmake",
        "-S",
        str(REPO_ROOT),
        "-B",
        str(BUILD_DIR),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCODE_INSTRUMENTATION=COVERAGE",
        "-DSTEPWRIGHT_BUILD_BENCHMARKS=OFF",
        "-DFETCHCONTENT_QUIET=ON",
    ]
    cmd.extend(extra_cmake_args)
    run(cmd)


def build(jobs: int) -> None:
    info("building stepwright with coverage flags")
    run(["cmake", "--build", str(BUILD_DIR), "-j", str(jobs)])


def run_tests(timeout: int) -> None:
    info("running test suite")
    run(
        [
            "ctest",
            "--test-dir",
            str(BUILD_DIR),
            "--output-on-failure",
            "--timeout",
            str(timeout),
        ]
    )


# --------------------------------------------------------------------------
# lcov capture / filter / genhtml
# --------------------------------------------------------------------------


def capture(gcov_tool: str) -> None:
    info("capturing coverage data with lcov")
    cmd = [
        "lcov",
        "--capture",
        "--directory",
        str(BUILD_DIR),
        "--gcov-tool",
        gcov_tool,
        "--rc",
        "geninfo_unexecuted_blocks=1",
        "--rc",
        "branch_coverage=1",
        "--ignore-errors",
        LCOV_IGNORE,
        "--output-file",
        str(RAW_INFO),
    ]
    run(cmd)
    if not RAW_INFO.exists() or RAW_INFO.stat().st_size == 0:
        fail(f"lcov produced no data at {RAW_INFO}")


def filter_info() -> None:
    info("filtering out system / third-party paths")
    run(
        [
            "lcov",
            "--remove",
            str(RAW_INFO),
            *REMOVE_PATTERNS,
            "--ignore-errors",
            LCOV_IGNORE,
            "--output-file",
            str(INFO),
        ]
    )

    info("restricting to stepwright src/ and include/")
    run(
        [
            "lcov",
            "--extract",
            str(INFO),
            *EXTRACT_PATTERNS,
            "--ignore-errors",
            LCOV_IGNORE,
            "--output-file",
            str(INFO),
        ]
    )

    if not INFO.exists() or INFO.stat().st_size == 0:
        fail(
            "Filtered coverage info is empty. Either no stepwright sources were "
            "exercised by the tests, or the gcov tool failed to decode the "
            ".gcda files. Re-run with --clean and inspect the lcov output above."
        )

    run(["lcov", "--list", str(INFO), "--ignore-errors", LCOV_IGNORE])


def gen_html() -> None:
    info("generating HTML report")
    if HTML_DIR.exists():
        shutil.rmtree(HTML_DIR)
    HTML_DIR.mkdir(parents=True, exist_ok=True)
    run(
        [
            "genhtml",
            str(INFO),
            "--output-directory",
            str(HTML_DIR),
            "--demangle-cpp",
            "--legend",
            "--branch-coverage",
            "--title",
            "stepwright coverage",
            "--rc",
            "genhtml_branch_coverage=1",
            "--ignore-errors",
            LCOV_IGNORE,
        ]
    )


def open_report() -> None:
    index = HTML_DIR / "index.html"
    if not index.exists():
        warn(f"no index.html at {index}")
        return
    url = index.as_uri()
    info(f"opening {url}")
    webbrowser.open(url)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the stepwright code coverage pipeline end-to-end.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=cpu_count(),
        help="Parallel build jobs.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="Per-test timeout (seconds) passed to ctest.",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Wipe build-coverage/ before configuring.",
    )
    parser.add_argument(
        "--skip-tests",
        action="store_true",
        help="Skip configure/build/test and only re-run capture+genhtml on existing .gcda data.",
    )
    parser.add_argument(
        "--no-install",
        action="store_true",
        help="Do not auto-install lcov; fail with instructions instead.",
    )
    parser.add_argument(
        "--open",
        dest="open_report",
        action="store_true",
        help="Open the generated HTML report in the default browser.",
    )
    parser.add_argument(
        "--cmake-arg",
        action="append",
        default=[],
        help="Additional argument forwarded to cmake configure (repeatable).",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    ensure_cmake_ctest()
    ensure_lcov(auto_install=not args.no_install)

    if args.clean:
        clean_build_dir()

    if not args.skip_tests:
        configure(args.cmake_arg)
        wipe_gcda()
        build(args.jobs)
        run_tests(args.timeout)
    else:
        if not BUILD_DIR.exists():
            fail("--skip-tests was passed but build-coverage/ does not exist. Run without --skip-tests first.")
        info("--skip-tests: reusing existing build artifacts")

    # Resolved after configure so CMakeCache.txt exists and we know which
    # compiler emitted the .gcno files (Clang vs GCC vs AppleClang).
    gcov_tool = resolve_gcov_tool()

    capture(gcov_tool)
    filter_info()
    gen_html()

    index = HTML_DIR / "index.html"
    info(f"done. report: {index.as_uri()}")

    if args.open_report:
        open_report()

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except CoverageError as err:
        fail(str(err))
    except subprocess.CalledProcessError as err:
        fail(f"command failed (exit {err.returncode}): {' '.join(str(c) for c in err.cmd)}")
    except KeyboardInterrupt:
        fail("interrupted")
