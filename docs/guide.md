# stepwright user guide

stepwright composes asynchronous work using explicit state machines. It does not
provide sockets, files or an OS event loop. Your application owns the operations,
their data and their executors. Start with the [complete README demo](../README.md)
or [example 01](../examples/01_first_machine.cpp). Every C++ block on this page and
in the README is extracted, compiled and run by the test suite, not maintained as
an unchecked duplicate. The [examples](../examples/README.md) are tested too.

## Tasks, completions and machines

A **task** is an operation started with a nonempty **completion** and optional
cancel token. A completion is just a noexcept function pointer plus context; it
borrows the error for that call. A task completes exactly once, inside `start`
or later on any thread. Start only idle tasks. Keep tasks, child tasks, callbacks,
wakers and referenced services alive until completion. Tasks have identity and
are not movable; use stable storage such as members, `optional`, `deque` or owned
pointers rather than growing a vector of live tasks.

Derive from `machine<YourType>` and define `step begin()`. Each step returns a
transition. Steps do not call each other to suspend: the driver carries out the
transition after the step returns. The driver loop, not tail-call optimisation,
keeps synchronous chains at constant stack depth. Steps are statically selected
by member-function-pointer template arguments, while the task start protocol is
polymorphic. Keep step functions accessible to the base (public, or friend it).

| Transition | Meaning |
|---|---|
| `done()` | Complete successfully. |
| `fail(e)` | Complete with e (empty e is success), adding failure context. |
| `then<&D::f>()` | Run f in the same loop with an empty error. |
| `after<&D::f>(child)` | Start child with this token; resume at f with its result. |
| `forward(child)` | Start child, then complete with its result. |
| `park<&D::f>()` | Wait for the current or next external wake. |
| `defer<&D::f>()` | Post f to the bound home executor; binding is required. |

A step is `step f()` or `step f(const stepwright::error&)`, optionally noexcept.
**A no-argument continuation is skipped on failure**: the driver propagates the
failure instead. An error-taking continuation explicitly handles it, including
cleanup or recovery. A transition must not refer to a child local to that step;
store the child in the machine, as all examples do.

Before the final receiver, `annotate(error&)` can add application context and
`on_complete(const error&)` can observe the outcome. They must not destroy the
machine: the engine still has to deliver its receiver. The receiver itself **may
destroy or restart the task**; implementations never touch it after delivery.
Never deliver completion, start children or call user callbacks under an internal
lock. Exceptions escaping steps, hooks or noexcept callbacks terminate, rather
than becoming task errors.

## Errors and context

An empty `error` is success and does no reference-count work. `error::make`
owns a `std::error_code` and message in one allocation; copies share a payload.
`is(errc)` checks a library code (`cancelled`, `timed_out`, `closed`,
`broken_promise`). Generic and application-specific error codes work too.
Each failing machine appends its type name; up to eight static-storage context
strings are retained and later drops counted. Context mutation is copy-on-write:
changing a copy does not change another thread's error. Distinct handles are safe
on different threads; mutation of the same handle needs synchronisation. Views
from `message` and `context` borrow the payload. `describe()` allocates a formatted
string. See [example 01](../examples/01_first_machine.cpp) for propagation.

```cpp
#include <stepwright/error.hpp>
int main() {
    auto original = stepwright::error::make(std::errc::invalid_argument, "bad record");
    auto copy = original;
    copy.add_context("decode_record"); // a string with static storage
    return original.context().empty() && copy.context().size() == 1 &&
                   copy.message() == "bad record" ? 0 : 1;
}
```

## Cancellation

Create a `cancel_source`, obtain a token, and pass it to the root's `start`.
Machines pass that token to children. Requests are one-way and cooperative,
not thread interruption: a running step must poll `cancel_requested()` if it
does long CPU work. Children decide how to stop. By default, cancellation wakes
a current park with `errc::cancelled`; running steps, child waits and deferrals
are not independently failed. Entering a later park rechecks an existing request.

`cancel_callback` owns an arbitrary nonthrowing callable registered on a token.
Requests run callbacks outside locks on the requesting thread; registering on an
already-requested token calls inline. Callback order is unspecified. Destruction
waits for a callback on another thread, but self-deregistration does not wait.
Distinct source/token copies share state and may be used concurrently; do not
assign/destroy a particular handle while another thread uses that same handle.

An `on_cancel()` machine hook replaces the default. It can run concurrently with
steps, including **before begin** for an already-requested token. It should cancel
external work, which eventually delivers its real result, or conditionally call
`wake_if_parked`. A hook that only tries a conditional wake before the first park
must also handle an already-requested token in its steps; custom hooks do not get
the default park-entry cancellation delivery. [Example 08](../examples/08_cancellation.cpp)
shows a socket-like adapter; it deliberately runs its hook on one thread.

```cpp
#include <stepwright/stepwright.hpp>
struct parked : stepwright::machine<parked> {
    step begin() { return park<&parked::resume>(); }
    step resume(const stepwright::error& e) { return fail(e); }
};
int main() {
    stepwright::cancel_source stop;
    parked op;
    bool cancelled = false;
    op.start({.fn = [](void* ctx, const stepwright::error& e) noexcept {
                  *static_cast<bool*>(ctx) = e.is(stepwright::errc::cancelled);
              }, .ctx = &cancelled}, stop.token());
    stop.request_cancel();
    return cancelled ? 0 : 1;
}
```

### Wake semantics

`wake(e)` buffers the first result for the current **or next** park, even during
a child wait or queued deferral. A wake cannot complete a child or bypass a queued
deferral. Duplicate wakes are dropped until consumption; a later wake may belong
to the next park. Unused buffered wakes are discarded at completion. Waking an
idle task is invalid. `wake_if_parked(e)` atomically requires a current park and
no pending result, returning whether it was accepted. Neither form owns lifetime;
an accepted wake can complete/destroy the machine inline. Stop producers before
destruction; conditional waking does not make stale pointers safe.

## Threading model and executors

`start` runs `begin` on its caller, even when the machine is bound. Later
continuations run on the delivering thread unless a home executor is bound and
the delivering thread is foreign. `bind` borrows an executor; set it while idle
and keep the executor alive. Binding does not mean a particular worker or a
serial strand. A machine's steps are exclusive, but its cancellation hook and
external producers may run concurrently. `defer` explicitly yields to the home;
an inline executor still uses the driver loop rather than recursive re-entry.

- `inline_executor`: immediate execution on the posting thread, no global
  serialisation of concurrent callers.
- `manual_executor`: thread-safe FIFO posting, `run_one`/`run_all` controlled by
  callers. Multiple consumers can run different work concurrently.
- `thread_pool`: fixed workers with local FIFO queues and work stealing.
  `size()` reports worker count (zero requested means one). `wait_idle()` waits
  for posted work and its nested posts, not future timer/I/O completions.
- `reschedule`: a task that hops to an executor using an embedded node.

`work_item` is an intrusive node with a noexcept `run` function. Never queue one
twice simultaneously; its address must remain stable until invocation. Posting
an existing node allocates nothing. `executor::post(callable)` is convenient but
allocates one owning node. Executors never touch a node after invoking it.
An executor that runs inline must report `running_in_this_thread()` for that call.

Finish pool-bound machines and external producers before `shutdown`. Shutdown
drains existing and nested work and joins, but outside posts after it begins are
invalid. Neither shutdown nor `wait_idle` may run on a pool worker. Blocking inside
an item is discouraged: queued work can run if a worker is idle, even behind a
blocked poster, but if **all workers block on queued work**, none can execute it.
Prefer transitions. The guard may take a few hundred microseconds plus scheduler
delay to notice a single item behind a blocked poster. See [runtime](design/runtime.md).

## Synchronisation primitives and combinators

All lock/wait operation objects must survive until completion. Keep primitives
alive through their waiters. State-changing calls deliver completions outside
locks through an allocation-free trampoline. A nested completion may therefore
run **after `set`/`unlock` returns**. Never block inside one waiting for work queued
behind that same callback on the same thread.

| Building block | Semantics and important lifetime rules |
|---|---|
| `event` / `wait()` | One-shot result signal for many FIFO waiters. Already set waits complete synchronously. Cancelled waiters unregister. Reset requires no waiters and no concurrent wait/set. |
| `shared_task` / `wait()` | First waiter starts inner; all share its result. Cancelling one waiter does not cancel inner. `cancel()` cancels inner; reset only after completion without concurrent use. |
| `async_mutex` | `lock()` returns a task, `try_lock()` is immediate. FIFO transfer with no barging. A grant winning a cancellation race still owns the lock and must unlock. Ownership is not thread-bound. |
| `async_shared_mutex` | Shared and exclusive lock tasks; queued writers stop new readers barging. `guarded` and `guarded_shared` release even when inner fails. |
| `when_all` / `when_all_range` | Start every child, wait for every completion. First failure in completion order wins; default `cancel_on_error` requests sibling cancellation. `run_all` does not cancel on child failure. Empty joins succeed. |
| `when_any` / `when_any_range` | First completion wins, cancel the others, **wait for them all**. `winner()` is valid after completion. Requires at least one child. |
| `finally_task` | Run inner, call nonthrowing cleanup, complete with inner's result. Callable cleanup is borrowed, not owned. |
| `make_task` / `fn_task` | Store a callable taking `(completion, cancel_token)` inline. It must implement exactly-once completion and the no-access-after-completion rule itself. |

Fixed joins store child pointers inline. Range joins borrow the pointer span too:
both the span's storage and tasks must survive all runs. Join construction allocates
internal cancellation state; large range joins allocate completion shards and range
races allocate result slots once. Children must be distinct idle tasks, not shared
concurrently among starts. [Fan-out](../examples/03_fan_out.cpp) and
[mutex](../examples/04_async_mutex.cpp) examples show composition and cleanup.

## Timers and deadlines

`delay(service, duration)` is a cancellable task; `with_deadline(inner, service,
timeout)` chooses inner, timeout or parent cancellation. A timeout requests inner
cancellation and waits for inner to finish before reporting `timed_out`. If inner
ignores cancellation it can take longer than the timeout: the deadline is not a
hard kill. No callback or child outlives the wrapper's completion.

`steady_timer_service` owns one background thread. Keep timer callbacks short;
bind consumer machines to an executor so heavy continuations leave that thread.
`testing::manual_timer_service` advances only in `advance`/`advance_to`.
Timer entries are intrusive. Equal deadlines fire in schedule order. Cancelling
an entry waits for a foreign callback; from its own callback it never waits.
Destroy services with no scheduled/firing entries, and never a steady service on
its own thread. See [example 02](../examples/02_timeouts.cpp).

## Streams

A `source<T>` is not a task. One consumer calls `try_pull()`, receiving item,
pending, end or failure. End/failure are sticky. After pending, wait on `ready()`
or `on_ready`; only after that completion pull again. At most one readiness wait
may be outstanding; do not pull concurrently with it. Readiness can be spurious,
so loop and retry. Cancel and complete any outstanding wait before destroying a
source. Items must be nothrow move-constructible and destructible.

- `channel<T>(capacity)`: multi-producer, single-consumer bounded queue.
  `try_send` refuses when full; `send` returns a cancellable FIFO task providing
  backpressure. The default constructor is unbounded and may allocate on growth.
  `close(reason)` drains buffered items, then ends/fails; queued sends report
  `closed`. Failed/cancelled sends retain their value.
- `merge`: fair rotating fan-in, arming all pending inputs rather than blocking
  behind the first. Ends after all inputs end; first polled failure is sticky.
  The merge exclusively consumes its inputs and borrows their lifetimes.
- `broadcast<T>`: one producer, many single-consumer subscriptions. Capacity is
  fixed; `overflow::wait` waits for slow readers, `drop_oldest` drops retained
  items and counts each subscriber's misses. Subscriptions start with the next
  publication, not history. Destroy subscriptions before the broadcast. Item
  copies/moves/destructors run under its lock and must not throw or re-enter it.
- `empty`, `just`, `from_range`, `generate`: ready sources. Ranges are borrowed
  lvalues and must remain unchanged; a generator returns `optional<T>` and ends
  on `nullopt`.
- `interval`: periodic tick numbers, coalescing missed ticks to the latest one.
- `for_each`: drain into a callback returning void, bool (false stops successfully)
  or error. It polls cancellation between items and cancels a pending wait.

```cpp
#include <stepwright/stepwright.hpp>
int main() {
    stepwright::channel<int> queue(2);
    queue.try_send(4);
    queue.try_send(5);
    queue.close(); // drain, then end
    int sum = 0;
    auto consume = stepwright::for_each(queue, [&](int n) { sum += n; });
    bool success = false;
    consume.start({.fn = [](void* ctx, const stepwright::error& e) noexcept {
                       *static_cast<bool*>(ctx) = !e;
                   }, .ctx = &success});
    return success && sum == 9 ? 0 : 1;
}
```

### Writing a source machine

`source_machine<D,T>` is lazy: first pull/readiness runs `begin`. Its transitions
are `then`, `after`, `park`, `defer`, `fail`, plus `emit<&D::next>(value)` and
`end()`. Emitting suspends production until the next pull, with at most one
asynchronously produced item buffered. There is no `forward`: a source emits
items rather than completing as a task. A no-argument step skips failures just
as in a task machine. [Example 07](../examples/07_source_machine.cpp) pages through
a backend without buffering the whole stream.

`set_token` separately controls the source run and its children. Cancelling a
consumer's readiness wait cancels **only that wait**, not the source's child or
buffered item. If a consumer stops early while a source run is suspended, cancel
the source token and drive/settle it before destruction; readiness cancellation
alone is not a source join. Destroy only before first pull, between nonsuspended
pulls or after end/failure, never during a child, park, deferral or readiness wait.

## Deterministic testing

`testing::virtual_executor` implements both executor and timer service. It starts
at virtual time zero; no callbacks run until driven. `run_until_idle` drains only
currently ready work. `run_for(d)` advances time when nothing is ready, up to d;
`run_until_quiescent` advances through all timers, so it never ends for an infinite
periodic program. FIFO preserves readiness order. Random mode chooses among ready
work using a seed; report that seed on failure. Same seed plus the same
single-threaded program gives the same interleaving. Concurrent foreign posts
make the input order nondeterministic. Do not call `run_*` concurrently.

```cpp
#include <stepwright/stepwright.hpp>
#include <chrono>
int main() {
    stepwright::testing::virtual_executor ex(stepwright::testing::schedule::random, 42);
    stepwright::delay wait(ex, std::chrono::seconds(5));
    bool done = false;
    wait.start({.fn = [](void* ctx, const stepwright::error& e) noexcept {
                   *static_cast<bool*>(ctx) = !e;
               }, .ctx = &done});
    ex.run_until_quiescent();
    return done && ex.now().time_since_epoch() == std::chrono::seconds(5) ? 0 : 1;
}
```

This explores scheduling interleavings, not memory-model reorderings or all real
thread races: keep TSan and real multi-thread stress tests. [Example 09](../examples/09_deterministic_testing.cpp)
finds a lost update across 200 seeds and replays the first failure.

## Debugging

Enable `STEPWRIGHT_TRACK_TASKS` through CMake for the **whole program**, then explicitly
include `<stepwright/debug/live_tasks.hpp>`. `debug::dump(stream)` prints type, state,
resume step, age and starting thread; `for_each_live` copies a snapshot before
calling your function. It allocates, may be immediately stale and is not
async-signal-safe. `live_count` counts current entries. With tracking off these
APIs report no live entries. [Example 10](../examples/10_debugging_hangs.cpp) shows
how a stuck `waiting_child` or `parked` step points to a missing signal.

Tracking changes machine/source-machine/transition **layout**. All translation
units exchanging those types must use the same definition; mixing tracked and
untracked code violates the one-definition rule. Do not toggle it for one source
file of a program. `STEPWRIGHT_DEBUG` enables precondition checks without changing
layout; configure it consistently too (default follows `NDEBUG`).

The optional `<stepwright/detail/log.hpp>` facade exposes `STEPWRIGHT_LOG_TRACE`,
`DEBUG`, `INFO`, `WARN`, `ERROR`. CMake's `STEPWRIGHT_ENABLE_LOGGING=ON` requires an
installed spdlog package; `STEPWRIGHT_LOG_LEVEL` sets its compile-time filter.
Disabled macros do not evaluate arguments. This is an instrumentation facade,
not automatic per-step tracing. It is not part of the stable public API and
logging may allocate or block. Prefer the registry and breakpoints first.

## Performance guidance

- Embed children, waiters and work items. Reuse operations only after completion;
  fixed containers avoid per-run storage churn.
- A cancellation source allocates state once (currently 1,280 bytes shared by a token tree),
  joins allocate at construction, deadline/shared-task sources on first run,
  bounded buffers at construction and pools at construction. These are not
  per-step allocations. Error creation/context copy-on-write/formatting,
  diagnostic snapshots and callable `post` may allocate. Unbounded channels
  allocate on growth. Your item types and callbacks may allocate too.
- Prefer coarse enough leaf work to amortise dispatch. A trivial fan-out can be
  slower on more workers because one starter posts everything and steals cause
  cache traffic. Do not infer scaling from step-only microbenchmarks.
- Use bounded queues for controlled memory/backpressure, and bind away from timer
  or I/O threads when continuations do expensive work.
- Measure with identical flags and workloads. The [results](benchmarks.md) report
  median and variation; the [method](../benchmarks/README.md) explains wall versus
  CPU time, coroutine elision and scheduling noise.

## Common pitfalls

1. **A local child dies when its step returns.** Store it in the operation.
2. **A completion destroys the operation.** Do not read it afterwards, even just
   to update a counter. Snapshot needed data before calling the receiver.
3. **An early reply arrives during a send.** Use buffered `wake`; a child wait
   and a park are independent channels. Use conditional waking for cancellation.
4. **Binding is not a start hop or strand.** `begin` runs on the starting thread;
   use `defer` for the first hop, and synchronise shared application state.
5. **Cancellation is not destruction or a join.** Request, then wait for completion
   before releasing storage. A deadline waits for cancellation-resistant children.
6. **Idle pool does not mean idle application.** A timer may still post later.
   Finish all foreign producers before shutting down the pool.
7. **Blocking exhausts workers.** Liveness with an idle worker is not permission
   to block every worker or wait inside a same-thread trampoline callback.
8. **Tracking is an ABI choice.** Never mix layouts across translation units.
9. **A query is not completion.** `is_set`, `is_done`, `is_closed`, queue sizes,
   cancellation flags and registry snapshots are not destruction signals. Observe
   the relevant completions with proper cross-thread synchronisation, and settle
   all other users before reclaiming storage. After observing the last item/timer
   callback, you may destroy its pool/steady timer service on another thread even
   if the publishing `post`/`schedule` has not returned. Do not start new outside
   posts during shutdown. Caller-driven `run_*`/`advance*` loops must return before
   their runtime is destroyed. See [teardown and lifetimes](design/runtime.md#teardown-and-lifetimes).

## FAQ

**Is it a coroutine library or sender implementation?** Neither. It is an explicit
task/step protocol. An adapter is a possible future direction, not shipped here.

**Can I restart an operation?** Task machines and the documented wait/join tasks
are restartable after completion, including from their receiver. Reset your own
result/counter members in `begin`. Sources are normally one-pass; do not assume a
completed stream resets itself.

**How do I return a value?** Keep it in the operation and read it after synchronising
with completion. The completion error is the error channel; it is not a type-erased
result container. Streams carry typed values in `pull<T>`.

**Can I block main until completion?** Yes, outside executors. See the
condition-variable helper in [example_support.hpp](../examples/example_support.hpp).
Its notification happens while holding its mutex so the waiting thread cannot
destroy the target before notification has finished. Do not copy it into a worker
that must run work needed for its own result.

**Are debug checks a recovery mechanism?** No. Preconditions apply in Release too.
Debug diagnoses active destruction, double start and invalid waits; disabling the
checks does not make those operations valid.

**Do I need exceptions or third-party runtime libraries?** Errors are values, but
construction/allocation can throw and the library is not promised to build with
exceptions disabled. Default use requires only the C++20 standard library and
threads. Testing/benchmark dependencies and optional logging are separate.

**Where is the API reference?** Run `make docs`; see the README's volume-copy command
for the generated HTML. Design arguments live in [design/](design/engine.md).
