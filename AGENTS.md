# Notes for contributors and coding agents

## Project
stepwright is a header-only C++20 library for writing asynchronous operations as
explicit state machines: each step is a member function that returns the next
transition (`done`, `fail`, `then`, `after`, `forward`, `park`, `defer`).
Start with `docs/overview.md` (purpose, design principles, architecture,
roadmap). Milestone specifications live in `.claude/plans/spec-*.md`.

## Supported platforms
macOS (AppleClang, native) and Linux (GCC 14, Clang 20 with libstdc++ or
libc++; x86-64 and arm64). Windows is not supported.

## Build and test
Linux always runs in the pinned dev container (`docker/Dockerfile`); macOS
runs natively. Both go through `scripts/ci.sh`, which CI uses too.

```bash
# macOS, native (needs Xcode command-line tools + CMake >= 3.21; Ninja optional)
make test          # Release build + tests
make debug         # Debug build + tests
make asan          # ASan + UBSan
make tsan          # ThreadSanitizer
make check         # forbidden scan + release + debug + asan + tsan

# Linux, in Docker (COMPILER=gcc|clang|clang-libcxx, PLATFORM=linux/amd64 for x86-64)
make docker-test
make docker-asan / docker-tsan / docker-ubsan / docker-leak
make docker-shell  # interactive shell in the container

# Tool-version-sensitive tasks always run in the container
make format        # clang-format 20, in place
make format-check
make tidy          # clang-tidy 20
make cppcheck
make coverage
```
Container build trees live in the Docker volume `stepwright-build-<platform>`
(`make docker-clean` deletes it); native build trees live in `build/`.
On macOS, CMake may need `export PATH="/opt/homebrew/bin:$PATH"`.

## Independent-implementation rules (mandatory)
- Implement from the specifications and public documentation only. Do not read,
  copy, paraphrase or consult any proprietary or third-party codebase whose
  license does not permit it, and do not reuse code from earlier private
  prototypes of this project.
- Public prior art (papers, standard proposals, open-source projects with
  compatible licenses) may inform designs; record it in `docs/provenance.md`.
- Every change that adds a component appends an entry to `docs/provenance.md`.

## Engineering rules
- Never start a child task, fire a completion, or invoke user callbacks while
  holding an internal lock.
- After invoking a completion, never touch the completing object again.
- No heap allocation on the per-step or per-wait hot paths.
- Class layout must not depend on `NDEBUG`; debug-only checks use
  `STEPWRIGHT_DEBUG` and keep members unconditionally.
- Every bug fix lands with a regression test.
