# Changelog

All notable changes to stepwright are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). The 0.x API may change between minor
versions. Move the Unreleased section to a dated version heading when releasing.

## [Unreleased]

## [0.2.1] - 2026-10-29

### Changed
- Error paths no longer allocate in steady state. `error::make` for a library
  code without a message (`cancelled`, `timed_out`, `closed`, `broken_promise`)
  returns a shared static payload that never allocates and is copied without
  reference counting, so deadline timeouts, cancellations and closes are
  allocation-free. Other payloads with messages up to 144 bytes, and context
  copy-on-write, reuse 256-byte blocks from per-thread magazines balanced by a
  bounded shared depot; longer messages allocate as before. Recycling is off
  under AddressSanitizer. `sizeof(error)` and the public API are unchanged.

### Fixed
- `stepwright::category()` is never destroyed. It used to be a function-local
  static destroyed at exit, so comparing or printing an error (including the
  shared cancellation value) during static destruction called members of a
  destroyed object (reported by UndefinedBehaviorSanitizer).

### Added
- Allocation tests for error paths (deadlines, default cancellation, joins,
  channel close, shared tasks, retry, cross-thread release, thread exit) and
  error benchmarks (`BM_error_code_only`, `BM_error_message_context`,
  `BM_error_cross_thread`, `BM_deadline_timeout`).

## [0.2.0] - 2026-10-28

### Added
- Stack-allocated, teardown-safe `sync_wait`, virtual-executor `run`, lvalue
  callable completions, and code/message `fail` helpers.
- Container constructors for range joins, sequential composition, and
  deterministic cancellable retry with capped exponential backoff.
- Reusable channel sends, broadcast publications, delays and deadline timeouts.
- A transition cheat sheet, shorter runnable examples, seven side-by-side
  learning programs, and helper allocation/stack/teardown tests and benchmarks.
- A README banner, architecture and machine-lifecycle diagrams linked to the
  source they describe, and a chart comparing benchmark results with familiar
  alternatives.

## [0.1.0] - 2026-10-27

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
