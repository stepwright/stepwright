# stepwright benchmarks

Google Benchmark (v1.9.1, fetched and pinned by `cmake/Dependencies.cmake`)
programs, one per layer. Published results: [`docs/benchmarks.md`](../docs/benchmarks.md).

## Running

```bash
make bench                                        # macOS, native Release
COMPILER=gcc make docker-bench                    # Linux, in the dev container
make bench BENCH_ARGS="--benchmark_filter=pool"   # a subset
make bench BENCH_OUT=build/bench/macos BENCH_LABEL="macOS (Apple M4 Max)"
COMPILER=gcc make docker-bench BENCH_OUT=build/bench/linux BENCH_LABEL="Linux arm64 (container)"
python3 scripts/bench_report.py build/bench/macos build/bench/linux --update docs/benchmarks.md
```

- Benchmarks always build in their own Release tree (`build/<compiler>-bench`),
  with the project's Release flags (`-O3 -DNDEBUG`, plus `-D_FORTIFY_SOURCE=2`
  from the toolchain files) and no sanitizer. Tests and examples are not built.
- `BENCH_OUT=<dir>` (relative to the repository root, also from the container)
  writes `bench_*.json` (Google Benchmark's JSON) and `context.json` (label, CPU,
  OS, compiler, the exact code-generation flags taken from
  `compile_commands.json`, git commit, arguments). `scripts/bench_report.py` turns
  one or more such directories into Markdown tables, one column pair per platform,
  grouped by benchmark file; `--update FILE` rewrites only the region between the
  `<!-- bench-report:begin -->` and `<!-- bench-report:end -->` markers.

## Method

- **Repetitions.** The default `BENCH_ARGS` are `--benchmark_min_time=1s
  --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`: each
  benchmark runs at least one second, five times, and the report gives the median
  of the five and the coefficient of variation (CV). The median resists the odd
  run disturbed by the operating system; a CV above a few percent means the number
  should be read as a range.
- **Unit.** Every benchmark reports items per second; the tables show nanoseconds
  per item (1e9 / items per second). What an item is (a step, a hand-off, a machine,
  a delivered message) is listed below.
- **Real time vs CPU time.** Single-threaded benchmarks use CPU time of the
  benchmark thread (Google Benchmark's default). Benchmarks involving other threads
  are registered with `UseRealTime()` (names end in `/real_time`, shown as
  "(real)"): the work happens on threads the CPU timer does not see, so wall-clock
  time is the honest measure. For those, ns/item is wall time divided by the items
  processed on *all* threads.
- **Same work, same flags.** A baseline does the same work as the stepwright
  benchmark next to it (same item counts, same data, same compiler and flags), and
  is written as a competent user would write it, not as a straw man. No benchmark
  is tuned for stepwright: no padding, pinning or warm-up that a user would not do.
- **Noise.** Single-threaded benchmarks are stable to about 1–2 %. Threaded ones
  depend on the scheduler: which cores the threads land on (performance or
  efficiency cores on Apple silicon; the virtual CPUs of Docker Desktop's Linux VM),
  whether workers were asleep when a round started, and other load on the machine
  (the tables record the load average). Expect 5–15 % run-to-run variation for
  them, and treat the SPSC thread benchmarks (`BM_channel_spsc_threads`,
  `BM_baseline_mutex_deque_spsc`, `BM_channel_bounded_backpressure`) as bimodal:
  they swing by up to 2× with how the two threads are scheduled. Compare versions
  with alternating runs on an idle machine, not across sessions.

## What each benchmark measures

### `bench_engine.cpp`: the machine engine (M1)

| Benchmark | Item | Notes and baseline |
|---|---|---|
| `BM_then_chain/1000` | one `then` step | Back-to-back steps in one driver loop. |
| `BM_after_sync_child/1000` | one `after` step on a child that completes inside `start` | Child start (virtual call), completion, continuation. |
| `BM_after_async_child_manual/1000` | one `after` step on a child completed from a `manual_executor` | Adds the post and the asynchronous resumption. |
| `BM_park_wake_same_thread/1000` | one `wake` + `park` on one thread | The wake is buffered and consumed at the park. |
| `BM_park_wake_ping_pong_threads` (real) | one wake from another thread and its acknowledgement | Cross-thread round trip, dominated by the cache-line transfer. |
| `BM_error_propagation_depth/8` | one failure through 8 forwarding machines | Error context added at each level. |
| `BM_baseline_fnptr_chain/1000` | one call through a function pointer | Lower bound for a dynamically dispatched step. |
| `BM_baseline_std_function_chain/1000` | one call through `std::function` | The callback-chain style stepwright replaces. |
| `BM_coro_then_chain/1000` | one step of a C++20 coroutine's loop | Coroutine baseline for `BM_then_chain` (below). |
| `BM_coro_after_sync_child/1000` | one `co_await` of a child coroutine that completes at once | Coroutine baseline for `BM_after_sync_child`, child visible to the compiler. |
| `BM_coro_after_opaque_sync_child/1000` | the same, child not visible (not inlinable) | The same without frame elision. |
| `BM_shared_token_complete/N` | completing one of N parked machines that share a cancellation token | Wake plus deregistration from the shared token (the registrations are made outside the timed region). |

**The coroutine baseline.** `bench_engine.cpp` defines a minimal lazy task
(`coro_task`), as a user without a coroutine library would write it: the frame
starts suspended; `co_await task` stores the awaiting coroutine as the
continuation and transfers to the child (symmetric transfer, so synchronous
chains use constant stack, as stepwright's driver loop does); `final_suspend`
transfers back to the continuation; the result is a `stepwright::error`, checked
after each `co_await` like a no-argument machine step; exceptions terminate. Each
benchmark iteration creates one root coroutine and runs it to completion on the
calling thread.

Caveats, for a fair reading:

- *Frames.* Every coroutine call creates a frame, which the compiler may allocate
  on the heap or elide (HALO) when it can see the callee and prove the frame does
  not outlive the caller. The promise counts heap frames and the benchmarks report
  `frames_per_item`: 0.001 means only the root frame (one per 1,000 items) was
  allocated, i.e. the child frames were elided; 1 means one heap frame per child.
  `BM_coro_after_sync_child` lets the compiler see the child (it elides); the
  `_opaque_` variant marks the child `noinline`, which is what happens when the
  child lives in another translation unit or behind an interface, and shows the
  cost of a frame allocation per child. stepwright's children are member objects,
  started through a virtual call, and never allocate.
- *Sequencing is free in a coroutine.* `BM_coro_then_chain` is a plain loop
  inside one coroutine body; stepwright's `then` returns a transition to the driver
  (two indirect calls). The comparison shows what explicit, inspectable steps
  cost against straight-line code, not a defect of either.
- *Not measured:* asynchronous resumption (stepwright's `BM_after_async_child_manual`
  has no coroutine counterpart because it would need an executor and awaiter
  design of its own), cancellation (C++20 coroutines have none built in) and
  frame sizes.

### `bench_primitives.cpp`: synchronisation primitives and combinators (M2)

| Benchmark | Item | Notes and baseline |
|---|---|---|
| `BM_event_set_wait/N` | one waiter released by `set` | N waiters registered, then released in one batch. |
| `BM_mutex_uncontended` | one lock + unlock of `async_mutex` | Baseline: `BM_baseline_std_mutex_uncontended` (`std::mutex` lock + unlock). |
| `BM_mutex_handoff_chain/1000` | one hand-off of the lock to the next queued machine | Runs through the hand-off trampoline on one thread. |
| `BM_mutex_contended_threads/4` (real) | one acquisition, 4 threads competing | Each thread spins on its own completion flag. |
| `BM_shared_mutex_readers/1000` | one reader admitted | Readers queued behind a writer, admitted together. |
| `BM_when_all_sync/N`, `BM_when_any_sync/N` | one child of a `when_all_range` / `when_any_range` | Children complete inside `start`. Baseline: `BM_baseline_atomic_countdown/N`, the same virtual starts with a hand-written atomic countdown and no tokens. |
| `BM_when_all_async_manual/1000` | one child completed from a `manual_executor` | |
| `BM_timer_schedule_cancel_manual`, `_steady` | one schedule + cancel | Pairing heap; the steady service also signals its thread when the entry becomes the earliest. |
| `BM_with_deadline_inner_first` | one `with_deadline` run whose inner task wins | Includes scheduling and cancelling the timer. |

### `bench_streams.cpp`: streams (M3)

| Benchmark | Item | Notes and baseline |
|---|---|---|
| `BM_channel_send_pull_same_thread/1000` | one `try_send` + `try_pull` | Baseline: `BM_baseline_mutex_deque_same_thread`, `std::mutex` + `std::deque` + condition variable doing the same operations. |
| `BM_channel_spsc_threads` (real) | one item from a producer thread to a consumer thread | Consumer waits for readiness when empty. Baseline: `BM_baseline_mutex_deque_spsc`, the same with a blocking pop. |
| `BM_channel_bounded_backpressure` (real) | one item through a 16-slot channel | Producer machine with `send_op` (waits when full) and `for_each` consumer, each on its own thread. |
| `BM_merge_pull/8` | one item pulled from a merge of 8 generators | Fair rotation. |
| `BM_broadcast_publish/N` | one item delivered to one subscriber | One publish, then N subscribers pull it. |
| `BM_source_machine_emit/1000` | one item emitted by a `source_machine` and pulled | Baseline: `BM_baseline_vector_loop/1000`, summing a `std::vector`. |
| `BM_for_each_generate/1000` | one item from `generate` through `for_each` | |

### `bench_runtime.cpp`: thread pool and virtual executor (M4, M5)

| Benchmark | Item | Notes and baseline |
|---|---|---|
| `BM_pool_post_run/T` (real) | one work item posted from outside to T workers | Baseline: `BM_baseline_pool_post_run/T`, the textbook pool (`std::mutex`, `std::condition_variable`, `std::deque<std::function<void()>>`, `notify_one` per post). |
| `BM_pool_post_from_worker/T` (real) | one item re-posted by a worker (T independent chains) | Aggregate over all workers. Baseline: `BM_baseline_pool_post_from_worker/8`. |
| `BM_pool_ping_pong` (real) | one round trip between two pool-bound machines | Two deferrals and two park/wake hand-offs. |
| `BM_pool_fan_out_fan_in/10000` (real) | one machine of a 10,000-child `when_all_range` on 8 workers | Each child defers once (one pool post) and completes. Started from a worker. |
| `BM_pool_fan_out_fan_in_one_thread/10000` (real) | the same on one worker | Reference for the scaling target (8 workers ≤ 1.5× this). |
| `BM_pool_fan_out_fan_in_work/10000`, `_one_thread` (real) | the same, each child doing about 1 µs of CPU work | Scaling with real work (target: 8 workers ≥ 5× faster than one). |
| `BM_leaf_work` | the work one child does in `_work` | A dependent multiply-add chain; reports what "about 1 µs" is on the machine. |
| `BM_virtual_executor_post_run` | one item posted and run | Deterministic test executor. |

The fan-out benchmarks measure the cost of structure, not of work: their leaves
do nothing, so a single starting worker posting 10,000 tiny items is the
bottleneck, and adding workers can only add contention. `docs/M5-performance.md`
explains what was done to keep 8 workers within 1.5× of one.
