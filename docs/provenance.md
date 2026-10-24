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

## Log
| Date | Milestone | Component(s) | Spec | Notes |
|---|---|---|---|---|
| 2026-10-24 | M0 | Build system, tooling, CI, formatting config, license | Project plan | Repository started fresh; build/CI scripts reviewed and adapted; formatting and lint configs written new. |
| 2026-10-24 | M0.5 | Linux dev container, single CI entry script, CI workflows | Project plan | Ubuntu 24.04 image with pinned GCC 14 / Clang 20 toolchain; `scripts/ci.sh` shared by local runs and CI; dependencies pinned by URL + SHA-256. |
| 2026-10-24 | M1 | Error values, completion, intrusive cancellation, executors, tasks, machine driver, type names, logging shim, tests and engine benchmarks | `.claude/plans/spec-M1-core.md` | Independently implemented from the milestone contract and project overview; no external implementation or public prior-art source consulted. Ownership and lifetime protocol documented in `docs/M1-concurrency.md`. |
| 2026-10-24 | M1 round 2 | Conditional park wakes, O(1) cancellation unlinking, child-completion fast path, lifetime checks, scalable stress tests and shared-token benchmark, API comments | M1 §12 and `review-M1.md` | Independent changes based on the review and supplied defect probes; no external implementation or prior-art source consulted. Protocol and lifetime reasoning updated in `docs/M1-concurrency.md`. |
| 2026-10-24 | M1 round 3 | Independent buffered wake/child/defer readiness, SC cancellable park handshake, request/reply regressions and updated API semantics | M1 §13 and review round 2 | Independent fixes based on the amended contract and supplied `reply_race` probe; no external implementation/prior-art source consulted. Updated ownership and SC-order proof in `docs/M1-concurrency.md`. |
| 2026-10-24 | M2 | Hand-off trampoline, waiter protocol, `event`, `shared_task`, `async_mutex`, `async_shared_mutex`, `when_all`/`when_any`, timer services (steady, manual), `delay`, `with_deadline`, `fn_task`/`make_task`, `finally_task`, `reschedule`, `cancel_source::reset`, tests and primitive benchmarks | `.claude/plans/spec-M2-primitives.md` | Implemented from the milestone contract; no implementation source code was consulted. Semantics follow the public descriptions already listed above (cppcoro's async event/mutex/shared task, P2300 `when_all` and stop tokens). The timer queue is a textbook pairing heap (Fredman, Sedgewick, Sleator and Tarjan, "The pairing heap: a new form of self-adjusting heap", Algorithmica 1986) with a sequence-number tie-break. Design and race analysis in `docs/M2-primitives.md`. |
