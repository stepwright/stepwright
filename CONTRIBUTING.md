# Contributing to stepwright

Thanks for helping. Please follow the [code of conduct](CODE_OF_CONDUCT.md).
For vulnerabilities use [private reporting](SECURITY.md), not a public issue.
For other bugs, include a small reproducer and your compiler/platform; for larger
features discuss the intended contract before implementing it.

## Set up and validate

The support floor is GCC 13, Clang 18 and AppleClang 16 with C++20. macOS runs
natively (Xcode command-line tools, CMake ≥3.21; Ninja optional). Linux and
version-sensitive quality tools run in `docker/Dockerfile`. Do not substitute
host-installed formatting or analysis tools for the dev image.

```bash
export PATH="/opt/homebrew/bin:$PATH"  # macOS, when needed
make test
make debug
make asan
make tsan
make ubsan
CMAKE_ARGS=-DSTEPWRIGHT_TRACK_TASKS=ON make debug
COMPILER=gcc make docker-test
COMPILER=clang make docker-test
COMPILER=clang-libcxx make docker-test
COMPILER=gcc13 make docker-test
COMPILER=clang18 make docker-test
COMPILER=gcc make docker-debug
make docker-asan
make docker-tsan
make docker-ubsan
make docker-leak
COMPILER=gcc make docker-tsan
PLATFORM=linux/amd64 COMPILER=gcc make docker-test
make format-check
make tidy
make cppcheck
make forbidden
make coverage
make docs
make consumer
make docker-consumer
```

Default container compiler is GCC; sanitizer jobs in CI select Clang explicitly.
TSan cannot run under x86-64 Docker emulation on Apple silicon; native x86-64 CI
checks that configuration. `STEPWRIGHT_STRESS_SCALE=5 make tsan` runs a longer soak.
Try to keep the default Debug/TSan suite under about a minute. Coverage must remain
at least 90%. `make format` formats in place. `JOBS` controls parallelism;
`CMAKE_ARGS` selects additional CMake options in isolated build trees. Container
build trees use named volumes; native trees are under `build/`.

`make consumer` tests a relocated install, FetchContent and `add_subdirectory`
without the library's test/example dependencies. `make docs` fails on undocumented
public entities and Doxygen errors. Every fenced C++ guide/README block is compiled
and executed as part of `make test`; keep each one a complete, self-contained
program or link a tested example instead. Do not silently copy snippets into tests.

## Engineering and review expectations

- Add a regression test with every bug fix. Include race, lifetime and allocation
  cases for concurrent changes. Run relevant sanitizers before requesting review;
  report anything you could not run rather than claiming it passed.
- Never start children, deliver completions or invoke user callbacks under
  internal locks. Never touch an operation after delivering its completion.
- No per-step/per-wait hot-path heap allocations. Layout must not depend on
  `NDEBUG`; tracking is a whole-program, explicitly documented layout option.
- Explain concurrency invariants and ordering changes in `docs/design/`.
- For performance changes, compare like-for-like baselines on an idle machine,
  save context and variation, and update the [method/results](benchmarks/README.md).
  Benchmarks use their own Release build; do not run builds concurrently with
  publication measurements.
- Contribute only code you wrote or have the right to contribute under MIT.
  Cite relevant public prior art in [provenance](docs/provenance.md). No proprietary
  implementation details or incompatible code may be introduced.

## Commits and sign-off

Use one logical change per commit; each commit should build and pass tests so
history remains bisectable. Conventional Commit subjects are imperative, at most
72 characters and have no trailing period: `fix(pool): preserve posted-item liveness`.
Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `style`,
`chore`. Wrap body text at 72 columns, explain what and why, and describe the
failure mode for a fix. A new component and its tests belong together.

Sign off each contribution with `git commit -s`, certifying the
[Developer Certificate of Origin 1.1](https://developercertificate.org/).
This records a `Signed-off-by: Your Name <your-email>` trailer; it is not a
cryptographic signature or a transfer of copyright. The sign-off means you have
the right to submit the contribution under the project's MIT license and accept
that the contribution and sign-off are public. Reviewers expect tests, validation
results and any compatibility/performance impact in the pull request description.
