# Notes for contributors and coding agents

## Project
stepwright is a header-only C++20 library for writing asynchronous operations as
explicit state machines: each step is a member function that returns the next
transition (`done`, `fail`, `then`, `after`, `forward`, `park`, `defer`).
Start with `docs/overview.md` (purpose, design principles, architecture,
roadmap), then `docs/guide.md` and `docs/design/` for the implementation protocols.

## Supported platforms
macOS (AppleClang 16 or newer, native) and Linux (GCC 13 or newer,
Clang 18 or newer; libstdc++ or libc++; x86-64 and arm64). The dev image
also tests GCC 14 and Clang 20. Windows is not supported.

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
make run EXAMPLE=01_first_machine   # one example (examples/README.md)
make bench BENCH_OUT=build/bench/macos   # benchmarks, also as JSON (benchmarks/README.md)

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
make docs          # Doxygen HTML, warnings are errors (container)
make consumer      # native install/FetchContent/add_subdirectory consumers
make docker-consumer
```
Container build trees live in the Docker volume `stepwright-build-<platform>`
(`make docker-clean` deletes it); native build trees live in `build/`.
On macOS, CMake may need `export PATH="/opt/homebrew/bin:$PATH"`.

`CMAKE_ARGS=-DSTEPWRIGHT_TRACK_TASKS=ON` builds everything with the live task
registry (`include/stepwright/debug/live_tasks.hpp`). It changes class layout and is
cached in the build tree, so give it its own tree (natively `BUILD_ROOT=build/track`;
in the container a build type not used otherwise, e.g. `BUILD_TYPE=RelWithDebInfo`).

`PLATFORM=linux/amd64` on Apple silicon runs under emulation: ordinary
builds, tests, ASan and UBSan work, but ThreadSanitizer cannot start there
(the emulator rejects the address-randomization change TSan needs). x86-64
TSan runs on native x86-64 CI runners.

## Contribution policy
- Contribute only code you wrote or have the right to contribute under MIT.
- Cite public prior art (papers, standards proposals and open-source designs)
  in `docs/provenance.md` when adding or redesigning a component.
- Sign off contributions with `git commit -s` (DCO); see `CONTRIBUTING.md`.

## Engineering rules
- Never start a child task, fire a completion, or invoke user callbacks while
  holding an internal lock.
- After invoking a completion, never touch the completing object again.
- No heap allocation on the per-step or per-wait hot paths.
- Class layout must not depend on `NDEBUG`; debug-only checks use
  `STEPWRIGHT_DEBUG` and keep members unconditionally.
- Every bug fix lands with a regression test.

## Commits
- One logical change per commit; every commit builds and passes its tests
  (history must stay bisectable).
- [Conventional Commits](https://www.conventionalcommits.org/) subjects:
  `type(scope): summary` in the imperative mood, at most 72 characters, no
  trailing period. Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`,
  `build`, `ci`, `style`, `chore`.
- A body (wrapped at 72 columns) explains what changed and why; reference the
  failure mode for fixes.
- A new component lands with its tests in the same commit; follow-up test
  suites, benchmarks and documentation may be separate commits.
