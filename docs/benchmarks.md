# stepwright benchmark results

These are measurements of the v0.1.0 release candidate (commit 49fdca1), not
performance guarantees. The tables are generated from [raw JSON](../benchmarks/results/2026-10-26/)
by `scripts/bench_report.py`. Each of the **63 cases per platform** runs for at
least one second, **ten repetitions**, with median ns/item and coefficient of
variation (CV). The [method and baselines](../benchmarks/README.md) define an
item and explain CPU versus wall time, storage policies and coroutine elision.

```bash
make bench BENCH_OUT=benchmarks/results/2026-10-26/macos BENCH_LABEL="macOS (Apple M4 Max, AppleClang 16)"
COMPILER=gcc make docker-bench BENCH_OUT=benchmarks/results/2026-10-26/linux BENCH_LABEL="Linux arm64 (container, GCC 14)"
python3 scripts/bench_report.py benchmarks/results/2026-10-26/macos benchmarks/results/2026-10-26/linux --update docs/benchmarks.md
```

## Conditions and interpretation

Both platforms used the same Apple M4 Max (12 performance and 4 efficiency
cores): native macOS/AppleClang 16, then Docker Desktop's Linux arm64 VM/GCC 14.
The suites ran **sequentially from a clean checkout of the measured commit**, with
no build, sanitizer, analysis or other container jobs running alongside them.
Headers and benchmark sources have the same SHA-256 in both contexts.

**Complete machine idleness could not be guaranteed.** Normal desktop/system
services remained enabled; no cores were pinned or isolated. The native starting
host load was 4.13 / 5.45 / 4.77, and the Linux run's host load was
3.04 / 4.55 / 4.72. Longer averages include the preceding native run.
The VM's own load (about 0.0) does not describe host activity. The platform
table uses the first executable's context; each raw executable JSON retains its
own date and load. Treat high-CV threaded numbers as ranges, not exact latencies.

Compare stepwright with its baseline **within a column**. Compiler, standard library
and OS differ between columns, even on the same physical hardware. Threaded
ns/item divides wall time by work done on all workers: for example 0.59 ns/item
on eight workers is aggregate throughput, not a sub-nanosecond task latency.

## What the run shows

- **Explicit steps:** a `then` is 1.74 ns natively and 2.64 ns with GCC; `after` on
  an immediately completing child is 3.98 / 5.22 ns. A function-pointer call is
  about 0.72 ns. The extra cost buys the transition, error/cancellation protocol
  and inspectable state; successful steps and waits do not allocate.
- **Coroutines are sometimes faster.** A visible child coroutine costs 1.35 ns
  with AppleClang when its frame is elided, versus 3.98 ns for `after`. Opaque
  children allocate one frame each (16.3 ns with AppleClang, 10.8 ns with GCC).
  GCC's visible child also allocates here. `frames_per_item` reports what happened;
  this does not measure asynchronous coroutine resumption or cancellation.
- **Primitive costs depend on the path.** An uncontended async mutex is
  3.71 / 3.13 ns versus 4.24 / 3.29 ns for `std::mutex`. A same-thread channel
  send+pull is 3.11 / 2.73 ns versus 10.5 / 8.11 ns for the mutex/deque baseline.
  The queued hand-off path is substantially more work than an uncontended lock;
  it runs the waiter/completion trampoline and machine continuation.
- **Pool structure is inexpensive, but not free.** Eight-worker outside posting
  is 19.8 / 16.0 ns/item in this run. The owning callback/deque baseline is much
  slower under contention; its storage policy is different from preallocated
  intrusive nodes and is documented in the method. Worker reposting and outside
  posting are different workloads, not interchangeable latency measurements.
- **Trivial fan-out does not benefit from more workers.** A 10,000-child join is
  22.8 ns/machine on eight workers versus 18.5 ns on one natively (1.23×), and
  24.6 versus 19.1 ns in Linux (1.29×). One worker still starts/posts everything;
  extra workers add cache traffic. With useful leaf work, eight workers are
  **8.5× faster natively and 6.5× faster in Linux**. The measured leaf work itself
  is about 1.05 / 0.54 µs; compare scaling against the same compiler's one-worker
  reference, not against the other platform.

## Noise and controlled comparisons

The highest CVs in this run are the native one-worker repost (17.1%), the Linux
ten-child `when_all` (18.2%) and the native eight-worker outside-post baseline
(9.4%). These are scheduler-sensitive cases.

The native two-thread SPSC channel measured 89.0 ns/item here, against 21.8 ns
in an earlier full run of the code before the final lifetime fixes, on the same
machine. Back-to-back runs of both versions now measure about 92 ns, so the
difference is run context (for example, macOS placing the two threads on
different core clusters), not a code change. Thread ping-pong numbers on Apple
silicon are ranges, not latencies; Linux measured 7.68 ns for the same benchmark.

The [controlled comparison data](../benchmarks/results/2026-10-25/comparisons/README.md)
and [performance design](design/performance.md) cover the pool's liveness cost
and the 1,280-byte versus 256-byte cancellation state. In the controlled pool
comparison the largest regression was 2.5% on macOS and 6.5% on Linux. Eight
cancellation stripes remain the default because the compact state was 2.31× /
1.81× slower with eight workers concurrently registering real parks, despite
winning the four-worker Linux comparison.

## Results

<!-- bench-report:begin -->
### Platforms

| | macOS (Apple M4 Max, AppleClang 16) | Linux arm64 (container, GCC 14) |
|---|---|---|
| CPU | Apple M4 Max | Apple M4 Max (container) |
| OS | Darwin 24.6.0 arm64 | Linux 7.0.14-linuxkit aarch64 |
| Compiler | Apple clang version 16.0.0 (clang-1600.0.26.6) | g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0 |
| Flags | `-O3 -DNDEBUG -D_FORTIFY_SOURCE=2 -std=c++20 -fno-omit-frame-pointer` | `-O3 -DNDEBUG -D_FORTIFY_SOURCE=2 -std=c++20 -fno-omit-frame-pointer -fmax-errors=3` |
| Commit | 49fdca1 | 49fdca1 |
| Source SHA-256 | a96cba8525d78b7eddf0ec4dd9a7d15031786365f7e5f0f3b25e4bcad56e8095 | a96cba8525d78b7eddf0ec4dd9a7d15031786365f7e5f0f3b25e4bcad56e8095 |
| Date | 2026-10-26T19:38:13+05:30 | 2026-10-26T14:24:34+00:00 |
| CPUs | 16 | 16 |
| Load average | 4.3 / 5.5 / 4.8 | 0.0 / 0.0 / 0.0 |
| Host load average | 4.13 / 5.45 / 4.77 | 3.04 4.55 4.72 |
| Arguments | `--benchmark_min_time=1s --benchmark_repetitions=10 --benchmark_report_aggregates_only=true` | `--benchmark_min_time=1s --benchmark_repetitions=10 --benchmark_report_aggregates_only=true` |

### `bench_engine`

| Benchmark | macOS (Apple M4 Max, AppleClang 16) ns/item | CV | Linux arm64 (container, GCC 14) ns/item | CV | Notes |
|---|---:|---:|---:|---:|---|
| `BM_then_chain/1000` | 1.74 | 0.4 % | 2.64 | 0.2 % |  |
| `BM_after_sync_child/1000` | 3.98 | 1.8 % | 5.22 | 2.2 % |  |
| `BM_after_async_child_manual/1000` | 13.5 | 0.7 % | 13.8 | 1.1 % |  |
| `BM_park_wake_same_thread/1000` | 7.71 | 0.6 % | 7.75 | 0.9 % |  |
| `BM_park_wake_ping_pong_threads (real)` | 140 | 2.9 % | 255 | 0.7 % |  |
| `BM_error_propagation_depth/8` | 13.8 | 0.1 % | 13.4 | 0.3 % |  |
| `BM_baseline_fnptr_chain/1000` | 0.72 | 0.2 % | 0.72 | 0.9 % |  |
| `BM_baseline_std_function_chain/1000` | 1.01 | 2.1 % | 0.98 | 1.1 % |  |
| `BM_coro_then_chain/1000` | 0.72 | 1.6 % | 0.25 | 0.3 % | frames_per_item = 0.001 / 0.001 |
| `BM_coro_after_sync_child/1000` | 1.35 | 1.2 % | 9.91 | 0.9 % | frames_per_item = 0.001 / 1 |
| `BM_coro_after_opaque_sync_child/1000` | 16.3 | 0.7 % | 10.8 | 0.9 % | frames_per_item = 1 / 1 |
| `BM_shared_token_complete/1000` | 9.00 | 0.5 % | 9.32 | 1.2 % |  |
| `BM_shared_token_complete/10000` | 8.86 | 0.1 % | 9.05 | 1.2 % |  |
| `BM_shared_token_complete/100000` | 8.95 | 0.2 % | 13.7 | 0.2 % |  |

### `bench_primitives`

| Benchmark | macOS (Apple M4 Max, AppleClang 16) ns/item | CV | Linux arm64 (container, GCC 14) ns/item | CV | Notes |
|---|---:|---:|---:|---:|---|
| `BM_event_set_wait/1` | 9.58 | 1.1 % | 10.3 | 0.9 % |  |
| `BM_event_set_wait/1000` | 7.21 | 1.1 % | 7.04 | 2.8 % |  |
| `BM_mutex_uncontended` | 3.71 | 0.6 % | 3.13 | 1.3 % |  |
| `BM_baseline_std_mutex_uncontended` | 4.24 | 0.6 % | 3.29 | 0.2 % |  |
| `BM_mutex_handoff_chain/1000` | 13.2 | 0.2 % | 14.3 | 1.2 % |  |
| `BM_mutex_contended_threads/4 (real)` | 208 | 1.3 % | 367 | 2.4 % |  |
| `BM_shared_mutex_readers/1000` | 10.4 | 1.6 % | 10.9 | 1.9 % |  |
| `BM_when_all_sync/10` | 4.11 | 0.1 % | 5.09 | 18.2 % |  |
| `BM_when_all_sync/1000` | 4.02 | 0.5 % | 3.62 | 0.6 % |  |
| `BM_baseline_atomic_countdown/10` | 1.73 | 3.6 % | 2.55 | 3.0 % |  |
| `BM_baseline_atomic_countdown/1000` | 1.84 | 1.0 % | 2.33 | 2.5 % |  |
| `BM_when_all_async_manual/1000` | 11.1 | 1.0 % | 13.2 | 0.6 % |  |
| `BM_when_any_sync/10` | 4.48 | 0.1 % | 5.23 | 3.3 % |  |
| `BM_timer_schedule_cancel_manual` | 4.13 | 0.1 % | 3.24 | 0.3 % |  |
| `BM_timer_schedule_cancel_steady` | 22.9 | 1.5 % | 28.8 | 1.3 % |  |
| `BM_with_deadline_inner_first` | 12.9 | 0.5 % | 14.9 | 0.5 % |  |

### `bench_runtime`

| Benchmark | macOS (Apple M4 Max, AppleClang 16) ns/item | CV | Linux arm64 (container, GCC 14) ns/item | CV | Notes |
|---|---:|---:|---:|---:|---|
| `BM_pool_post_run/1 (real)` | 8.33 | 0.3 % | 6.59 | 1.9 % |  |
| `BM_pool_post_run/4 (real)` | 12.6 | 0.9 % | 8.10 | 0.5 % |  |
| `BM_pool_post_run/8 (real)` | 19.8 | 2.5 % | 16.0 | 0.6 % |  |
| `BM_baseline_pool_post_run/1 (real)` | 47.0 | 2.3 % | 72.9 | 5.1 % |  |
| `BM_baseline_pool_post_run/4 (real)` | 96.7 | 0.5 % | 165 | 2.0 % |  |
| `BM_baseline_pool_post_run/8 (real)` | 1384 | 9.4 % | 7673 | 1.0 % |  |
| `BM_pool_post_from_worker/1 (real)` | 3.90 | 17.1 % | 4.09 | 0.1 % |  |
| `BM_pool_post_from_worker/2 (real)` | 1.99 | 0.3 % | 2.07 | 0.1 % |  |
| `BM_pool_post_from_worker/4 (real)` | 1.02 | 2.1 % | 1.07 | 0.3 % |  |
| `BM_pool_post_from_worker/8 (real)` | 0.59 | 6.3 % | 0.63 | 1.4 % |  |
| `BM_baseline_pool_post_from_worker/8 (real)` | 65.9 | 0.7 % | 74.1 | 1.0 % |  |
| `BM_pool_ping_pong (real)` | 39.5 | 0.4 % | 32.3 | 0.2 % |  |
| `BM_pool_fan_out_fan_in/10000 (real)` | 22.8 | 2.3 % | 24.6 | 3.2 % |  |
| `BM_pool_fan_out_fan_in_one_thread/10000 (real)` | 18.5 | 0.9 % | 19.1 | 0.4 % |  |
| `BM_pool_fan_out_fan_in_work/10000 (real)` | 165 | 0.4 % | 87.8 | 1.0 % |  |
| `BM_pool_fan_out_fan_in_work_one_thread/10000 (real)` | 1406 | 1.5 % | 567 | 0.9 % |  |
| `BM_leaf_work` | 1054 | 0.7 % | 539 | 0.8 % |  |
| `BM_virtual_executor_post_run` | 4.37 | 0.2 % | 3.37 | 0.5 % |  |
| `BM_shared_token_park_workers/1 (real)` | 59.8 | 0.4 % | 260 | 0.8 % | state_bytes = 1.28e+03 / 1.28e+03 |
| `BM_shared_token_park_workers/4 (real)` | 116 | 0.8 % | 216 | 0.3 % | state_bytes = 1.28e+03 / 1.28e+03 |
| `BM_shared_token_park_workers/8 (real)` | 176 | 1.2 % | 269 | 2.7 % | state_bytes = 1.28e+03 / 1.28e+03 |

### `bench_streams`

| Benchmark | macOS (Apple M4 Max, AppleClang 16) ns/item | CV | Linux arm64 (container, GCC 14) ns/item | CV | Notes |
|---|---:|---:|---:|---:|---|
| `BM_channel_send_pull_same_thread/1000` | 3.11 | 0.1 % | 2.73 | 0.5 % |  |
| `BM_baseline_mutex_deque_same_thread/1000` | 10.5 | 0.1 % | 8.11 | 1.0 % |  |
| `BM_channel_spsc_threads (real)` | 89.0 | 2.3 % | 7.68 | 0.7 % |  |
| `BM_baseline_mutex_deque_spsc (real)` | 43.1 | 8.1 % | 37.7 | 2.8 % |  |
| `BM_channel_bounded_backpressure (real)` | 94.2 | 2.2 % | 69.9 | 0.6 % |  |
| `BM_merge_pull/8` | 1.54 | 3.7 % | 1.45 | 2.0 % |  |
| `BM_broadcast_publish/1` | 4.23 | 0.7 % | 5.57 | 1.0 % |  |
| `BM_broadcast_publish/16` | 2.67 | 0.3 % | 4.08 | 6.7 % |  |
| `BM_broadcast_publish/1000` | 2.58 | 0.5 % | 3.33 | 0.5 % |  |
| `BM_source_machine_emit/1000` | 3.31 | 0.7 % | 3.89 | 5.6 % |  |
| `BM_for_each_generate/1000` | 1.83 | 0.7 % | 1.86 | 0.8 % |  |
| `BM_baseline_vector_loop/1000` | 0.65 | 6.2 % | 0.25 | 2.0 % |  |
<!-- bench-report:end -->
