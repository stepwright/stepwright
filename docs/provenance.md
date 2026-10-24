# Provenance

stepwright is written from scratch against its own design specifications. This file
records, per component, what was built and which public prior art informed it.

## Public prior art consulted
- Boost.Asio / Networking TS: composed asynchronous operations, completion
  handlers, executors and strands.
- cppcoro (Lewis Baker): `async_manual_reset_event`, `shared_task`,
  `async_mutex` naming and semantics.
- P2300 `std::execution` / libunifex: `when_all`, stop tokens and structured
  cancellation.
- Rust `Future`/`poll` and its compiler-generated state machines.
- Go channels: bounded/unbounded channels, close semantics.
- Classic "trampolined style" / driver-loop interpreters for stack-safe
  continuation chains.
- Work stealing: Blumofe and Leiserson, "Scheduling Multithreaded Computations
  by Work Stealing" (JACM 1999); Dmitry Vyukov, "Scalable Go Scheduler Design
  Doc" (2012); Carl Lerche, "Making the Tokio scheduler 10x faster" (Tokio blog,
  2019).
- splitmix64: Steele, Lea and Flood, "Fast Splittable Pseudorandom Number
  Generators" (OOPSLA 2014).

## Log
| Date | Milestone | Component(s) | Spec | Notes |
|---|---|---|---|---|
| 2026-10-24 | M0 | Build system, tooling, CI, formatting config, license | Project plan | Repository started fresh; build/CI scripts reviewed and adapted; formatting and lint configs written new. |
| 2026-10-24 | M0.5 | Linux dev container, single CI entry script, CI workflows | Project plan | Ubuntu 24.04 image with pinned GCC 14 / Clang 20 toolchain; `scripts/ci.sh` shared by local runs and CI; dependencies pinned by URL + SHA-256. |
| 2026-10-24 | M1 | Error values, completion, intrusive cancellation, executors, tasks, machine driver, type names, logging shim, tests and engine benchmarks | `.claude/plans/spec-M1-core.md` | Independently implemented from the milestone contract and project overview; no external implementation or public prior-art source consulted. Ownership and lifetime protocol documented in `docs/M1-concurrency.md`. |
| 2026-10-24 | M1 round 2 | Conditional park wakes, O(1) cancellation unlinking, child-completion fast path, lifetime checks, scalable stress tests and shared-token benchmark, API comments | M1 §12 and `review-M1.md` | Independent changes based on the review and supplied defect probes; no external implementation or prior-art source consulted. Protocol and lifetime reasoning updated in `docs/M1-concurrency.md`. |
| 2026-10-24 | M1 round 3 | Independent buffered wake/child/defer readiness, SC cancellable park handshake, request/reply regressions and updated API semantics | M1 §13 and review round 2 | Independent fixes based on the amended contract and supplied `reply_race` probe; no external implementation/prior-art source consulted. Updated ownership and SC-order proof in `docs/M1-concurrency.md`. |
| 2026-10-24 | M2 | Hand-off trampoline, waiter protocol, `event`, `shared_task`, `async_mutex`, `async_shared_mutex`, `when_all`/`when_any`, timer services (steady, manual), `delay`, `with_deadline`, `fn_task`/`make_task`, `finally_task`, `reschedule`, `cancel_source::reset`, tests and primitive benchmarks | `.claude/plans/spec-M2-primitives.md` | Implemented from the milestone contract; no implementation source code was consulted. Semantics follow the public descriptions already listed above (cppcoro's async event/mutex/shared task, P2300 `when_all` and stop tokens). The timer queue is a textbook pairing heap (Fredman, Sedgewick, Sleator and Tarjan, "The pairing heap: a new form of self-adjusting heap", Algorithmica 1986) with a sequence-number tie-break. Design and race analysis in `docs/M2-primitives.md`. |
| 2026-10-24 | M3 | `source<T>`/`pull<T>` protocol, `source_machine`, `channel`, `merge`/`merge_range`, `broadcast`, `empty`/`just`/`from_range`/`generate`/`interval`, `for_each`; shared `detail::wait_core`; internal-only `cancel_source` reset; permanent chaos test; stream benchmarks | `.claude/plans/spec-M3-streams.md` | Implemented from the milestone contract; no implementation source code was consulted. The pull/readiness split follows the public description of Rust's `Stream::poll_next`/waker model already listed above; channel close semantics follow Go channels (listed above). The broadcast ring with per-slot reader counts is an independent design. Design and race analysis in `docs/M3-streams.md`. |
| 2026-10-24 | M4 | `thread_pool`, `testing::virtual_executor`, live task registry (`debug/live_tasks.hpp`, `STEPWRIGHT_TRACK_TASKS`), `detail::value_name`; immortal `detail::cancelled_error()`; scenario, pool, virtual-executor, registry, static-lifetime and allocation tests; runtime benchmarks | `.claude/plans/spec-M4-runtime.md` | Implemented from the milestone contract; no implementation source code was consulted. Public designs relied on: per-worker queues with stealing of half a victim's queue (Blumofe and Leiserson); a bounded per-worker ring with overflow of half to a global injection queue, a cap on concurrently searching workers, the "last searcher wakes a sleeper" rule and not waking for a worker's own next item (the Tokio scheduler article); a periodic global-queue check every 61 ticks and delayed stealing of a worker's single next item (Vyukov's Go scheduler design doc). The idle protocol's ordering argument, the `wait_idle` counters and the virtual executor are independent designs; its generator is splitmix64 (Steele, Lea and Flood). Design and race analysis in `docs/M4-runtime.md`. |
