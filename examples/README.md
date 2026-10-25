# stepwright examples

Small, complete programs, each about a hundred lines with comments explaining
what happens and why. Read them in order: each one builds on the previous ones.

| Example | Shows |
|---|---|
| [`01_first_machine`](01_first_machine.cpp) | A machine: `begin`, `then`, `after` on a child, `fail`, and the context an error collects on its way up |
| [`02_timeouts`](02_timeouts.cpp) | `delay` and `with_deadline` on a `steady_timer_service`: a reply in time, and a timeout that cancels the slow work |
| [`03_fan_out`](03_fan_out.cpp) | `when_all` over machines bound to a `thread_pool`: the first error wins and the siblings are cancelled |
| [`04_async_mutex`](04_async_mutex.cpp) | Machines sharing state with `async_mutex::guarded`, with no thread ever blocked |
| [`05_channel_pipeline`](05_channel_pipeline.cpp) | Producers → bounded `channel` → `merge` → `for_each`, and how backpressure works |
| [`06_broadcast`](06_broadcast.cpp) | One publisher, several subscribers: `overflow::wait` versus `overflow::drop_oldest` |
| [`07_source_machine`](07_source_machine.cpp) | A paginated stream written as a `source_machine` that awaits a child per page |
| [`08_cancellation`](08_cancellation.cpp) | `cancel_source`, propagation to children, `on_cancel`, `wake_if_parked`, `cancel_callback` |
| [`09_deterministic_testing`](09_deterministic_testing.cpp) | `testing::virtual_executor`: virtual time, a race found over many seeds and replayed from one |
| [`10_debugging_hangs`](10_debugging_hangs.cpp) | A stuck machine found with the live task registry's `dump()` (built with task tracking) |

`example_support.hpp` holds the one helper they share: letting `main()` wait
for a task (never do that inside a step).

## Running

```bash
make run EXAMPLE=01_first_machine            # macOS, native
COMPILER=gcc make docker-run EXAMPLE=03_fan_out
make test                                    # every example is also a test
```

Each example is a CTest test named `example_<name>`: it must exit with status 0
and print, in order, every line listed in the `// expect:` comments at the end of
its source (`check_example.cmake` does the check). The output is deterministic:
no timings, no thread identities, no addresses.

## Patterns to copy

- **Bind machines to an executor** (`bind(pool)`) when their completions arrive
  from other threads (timers, other pools, I/O): the next step then runs on the
  pool, not on the thread that happened to deliver the result.
- **Never block in a step.** Wait with a transition instead: `after(child)` for
  a task, `park()` for an external `wake()`, `defer()` to yield to the executor.
- **Children are members.** A machine owns the tasks it starts (or
  `std::optional` of them when they need per-run arguments), so starting them
  never allocates and their lifetime is obvious.
- **Pass cancellation down, not around.** Start the root with a token; machines
  hand their token to every child. Give a machine an `on_cancel()` hook only
  when it must stop external work, and use `wake_if_parked()` in it.
