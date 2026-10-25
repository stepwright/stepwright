# Changelog

All notable changes to stepwright are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). The 0.x API may change between minor
versions. Move the Unreleased section to a dated version heading when releasing.

## [Unreleased]

### Added
- Initial v0.1.0 library: header-only C++20 explicit task and source machines,
  constant-stack transitions, completion targets and contextual errors-as-values.
- Cooperative cancellation tokens/callbacks, events, shared tasks, asynchronous
  mutexes, structured fan-out joins, deadlines, timers and task adapters.
- Pull streams, bounded/unbounded channels, fair merging, broadcasting,
  source factories and consuming tasks.
- Work-stealing thread pool, deterministic virtual executor, manual time and
  opt-in live task diagnostics.
- Runnable examples, compiled guide snippets, design arguments, allocation/race/
  stack tests, sanitizer/static-analysis CI and reproducible benchmark reports.
- CMake install/export and pkg-config metadata, relocation/FetchContent/subdirectory
  consumer tests; GCC 13, Clang 18 and AppleClang 16 support floors.
- Contributor, conduct and private vulnerability-reporting policies.

### Fixed
- Queued pool work can progress on an idle worker even when its posting item
  blocks waiting for it. A bounded-duty guard protects single-item liveness
  without polling a completely idle pool.

### Changed
- Documented the measured memory/contention trade-off of striped cancellation
  state, and made developer-only dependencies independent of installed consumers.
