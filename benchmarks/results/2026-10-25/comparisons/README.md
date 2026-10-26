# Controlled hardening comparisons

Raw Google Benchmark aggregate JSON for the memory and liveness comparisons in
[performance design](../../../../docs/design/performance.md). Both platforms use
the same Apple M4 Max: native AppleClang 16 on macOS and GCC 14 in the Linux arm64
dev container. No build or other container jobs ran during these measurements.
These short controlled runs are separate from the full ten-repetition publication
suite in the sibling `macos` and `linux` directories.

Each case uses `--benchmark_min_time=0.5s --benchmark_repetitions=7
--benchmark_report_aggregates_only=true`, real-time item rates, the project's
Release flags (`-O3 -DNDEBUG -D_FORTIFY_SOURCE=2 -std=c++20` and the same warning/
frame-pointer flags on each side). Per-file contexts retain the measured load,
date, executable and CPU topology. Compare within a platform, not across VMs.

## Pool

`pool-{macos,linux}-{before,after}.json` cover worker reposting, ping-pong, trivial
fan-out and working fan-out. The before pool is commit `d9be96b`, archived into a
separate build directory inside this repository; the after pool is the hardened
working tree used for publication. Run the runtime executable with filter
`BM_pool_(post_from_worker|ping_pong|fan_out_fan_in)` on both builds. The source
archive is a build fixture, not a second implementation shipped with the library.

`pool-linux-worker8-{before,after}.json` is an additional fresh-process check with
the publication settings (ten repetitions, at least one second). A full-suite
Linux worker-repost result was noisy; this direct comparison measured 0.608 ns/item
before and 0.611 after (+0.5%, both about 0.6% CV). Both the original full-suite
result and this confirmation are retained; no measurements were substituted.

## Cancellation state

`cancel-{macos,linux}-{striped,compact}.json` use the same runtime benchmark source
and pool, filtered to `BM_shared_token_park_workers`. Each worker starts and wakes
128 machines on one shared token after a concurrent start gate. The timed region
includes dispatch, registration and deregistration, not construction.

To reproduce the compact comparison in an isolated copy, change only cancellation
state layout/selection: one stripe; remove alignment from the stripe itself;
retain separate 128-byte-aligned reference-count and request/bookkeeping regions;
select `state.stripes[0]` instead of hashing. `state_bytes` verifies the resulting
256-byte state, versus 1,280 bytes for the default eight stripes. Do not add a
consumer-visible layout-selection macro or allocate stripes during registration.

Scheduling/core frequency and the fixed dispatch overhead affect these small
batches. The eight-worker result motivates the default; the four-worker Linux
compact result is faster and is reported rather than omitted.
