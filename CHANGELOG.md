# Changelog

All notable changes to stepwright are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). The 0.x API may change between minor
versions. Move the Unreleased section to a dated version heading when releasing.

## [Unreleased]

### Added
- First public release: header-only C++20 explicit task and source machines,
  constant-stack transitions, completion targets and contextual errors-as-values.
- Cooperative cancellation tokens/callbacks, events, shared tasks, asynchronous
  mutexes, structured fan-out joins, deadlines, timers and task adapters.
- Pull streams, bounded/unbounded channels, fair merging, broadcasting,
  source factories and consuming tasks.
- Work-stealing thread pool, deterministic virtual executor, manual time and
  opt-in live task diagnostics.
- Documented lifetime rules: observing a completion makes it safe to destroy the
  object, including a pool or timer service whose post or schedule call has not
  yet returned.
- Runnable examples, compiled guide snippets, design arguments, allocation/race/
  stack/teardown tests, sanitizer/static-analysis CI and reproducible benchmark
  reports.
- CMake install/export and pkg-config metadata without developer-only
  dependencies, relocation/FetchContent/subdirectory consumer tests; GCC 13,
  Clang 18 and AppleClang 16 support floors.
- Contributor, conduct and private vulnerability-reporting policies.
