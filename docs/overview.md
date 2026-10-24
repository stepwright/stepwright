# stepwright overview

## What stepwright is

stepwright is a header-only C++20 library for writing asynchronous operations as
explicit state machines. An operation is a class; each *step* is an ordinary
member function that returns a *transition* saying what happens next. A small
*driver loop* in the base class carries the transitions out.

```cpp
struct fetch_and_process : stepwright::machine<fetch_and_process> {
    step begin() { return after<&fetch_and_process::on_fetched>(fetch_); }

    step on_fetched(const stepwright::error& e) {
        if (e) return fail(e);
        process_.emplace(fetch_.data());
        return forward(*process_);          // finish with the child's result
    }

    fetch_op fetch_;
    std::optional<process_op> process_;
};
```

It is aimed at systems code (storage, networking, schedulers, embedded
runtimes) that needs predictable performance, explicit control flow that is
easy to debug, and no hidden allocations, and that either cannot or prefers
not to use C++20 coroutines.

### How it compares
- **C++20 coroutines:** convenient, but frame allocation, lifetime and
  cancellation are implicit. stepwright's state lives in plain members, you can
  inspect it in a debugger, and it never allocates per step.
- **Senders/receivers (P2300):** very general and composable, but heavy on
  templates. stepwright is deliberately small, with one execution model.
- **Callback chains / `std::function`:** easy to start with, but they grow
  into nested lambdas, allocate, and make error handling ad hoc. stepwright
  keeps the same "callback" efficiency with structure.

## Design principles

1. **Zero allocation on hot paths.** No heap allocation per step, per child
   wait, per park/wake, per deferral, or per primitive wait (waiter nodes are
   embedded in the waiting object).
2. **Static dispatch.** Step functions are template arguments
   (member-function pointers); no virtual call or `std::function` per step.
3. **Stack safety by construction.** Back-to-back steps and synchronously
   completing children are run by a loop, never by recursion, in every build
   mode, without relying on tail-call optimisation.
4. **Explicit threading model.** A machine continues on the thread that
   delivered its result, unless it is bound to a home executor, in which
   case continuations are posted there. The invariant that matters is
   exclusivity (one thread runs a machine's steps at a time), not thread
   identity.
5. **Structured cancellation.** Every start carries a cancel token; machines
   pass it to their children automatically; waiters unregister on cancel.
6. **Errors are values.** `stepwright::error` is a cheap, copyable handle around
   a `std::error_code`, a message and automatically added context frames
   (which machine the error passed through). Exceptions are not used as an
   error channel.
7. **Safe completion discipline.** Never start a child, fire a completion or
   call user code while holding an internal lock; never touch an object
   after delivering its completion (the receiver may destroy it).
8. **Same layout in every build.** Debug checks never change class layout.
9. **Every bug gets a regression test; every performance claim gets a fair,
   reproducible benchmark.**

## Architecture

```
┌──────────────────────────── streams ────────────────────────────┐
│ source<T> · source_machine · channel · merge · broadcast · ...   │
├──────────────────────────── primitives ─────────────────────────┤
│ event · shared_task · async_mutex · async_shared_mutex           │
│ when_all · when_any · with_deadline · make_task · finally · delay│
├──────────────────────────── runtime ────────────────────────────┤
│ executor · inline/manual executors · thread_pool · timer_service │
│ virtual_executor (tests) · live task registry (debug)            │
├──────────────────────────── core ───────────────────────────────┤
│ error · completion · cancel · task · machine<D> (driver loop)    │
└──────────────────────────────────────────────────────────────────┘
```

### Core vocabulary
- **task**: anything that can be started with a completion target and a
  cancel token, and completes exactly once (synchronously or later, on any
  thread).
- **machine<D>**: CRTP base for tasks written as steps. Entry step: `begin()`.
- **Transitions:** `done()`, `fail(e)`, `then<&D::f>()` (next step, same
  loop), `after<&D::f>(child)` (start a child, continue with its result),
  `forward(child)` (finish with the child's result), `park<&D::f>()` (wait for
  an external `wake(e)`), `defer<&D::f>()` (continue later on the home
  executor).
- **completion**: a function pointer plus context, called with
  `const error&`.

## Roadmap (v0.1)

| Milestone | Scope |
|---|---|
| M0 | Build system, tooling, CI, license |
| M0.5 | Linux dev container, `scripts/ci.sh`, CI workflows |
| M1 | Core: `error`, `completion`, `cancel`, `task`, `machine<D>`, `executor` with inline and manual executors, type names, logging shim, engine benchmarks |
| M2 | Primitives: `event`, `shared_task`, `async_mutex`, `async_shared_mutex`, `when_all`, `when_any`, timer services (steady and manual), `delay`, `with_deadline`, `make_task`, `finally_task`, `reschedule` |
| M3 | Streams: `source<T>` pull protocol, `source_machine`, `channel`, `merge`, `broadcast`, `empty`/`just`/`from_range`/`generate`/`interval` |
| M4 | Runtime: `thread_pool`, `virtual_executor` for deterministic tests, live task registry |
| M5 | Examples |
| M6 | Benchmarks (single- and multi-threaded, fair baselines, documented method) |
| M7 | Documentation, CMake install/export, packaging |
| M8 | v0.1.0 release |

Each milestone has its own detailed specification; later milestones build on
the core's guarantees (zero allocation, stack safety, threading and
cancellation semantics), so those must hold exactly as specified.

## Non-goals (v0.1)
- Requiring coroutines (a coroutine adapter may come later).
- Windows support.
- I/O, networking or OS event loops (stepwright composes them; it doesn't
  provide them).
- Exceptions as the error channel.

## Platforms
macOS (AppleClang, native builds) and Linux (GCC 14 and Clang 20 with
libstdc++ or libc++, x86-64 and arm64, built in the dev container).
