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
