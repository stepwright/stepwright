# M4 runtime: thread pool, virtual executor, live task registry

Independent design from the M4 specification (`.claude/plans/spec-M4-runtime.md`)
on top of M1–M3 (`docs/M1-concurrency.md`, `docs/M2-primitives.md`,
`docs/M3-streams.md`). No earlier guarantee is weakened: with
`STEPWRIGHT_TRACK_TASKS` off (the default) every M1–M3 type keeps its layout and
code path byte for byte.

## Carry-over from M3: an immortal `cancelled_error()`

`detail::cancelled_error()` returns a shared `errc::cancelled` handle so that
routine cancellation paths (stream readiness waits) do not allocate. It used to
be a function-local `static const error`, destroyed at exit; a thread still
running during static destruction could then copy a handle whose payload had
been freed. The handle is now placement-constructed once in static storage and
never destroyed (the function-local static is a trivially destructible
pointer, so no exit-time destructor is registered). The payload stays reachable
through that storage, so LeakSanitizer does not report it.

`test_static_lifetime` checks it where it matters: an object constructed before
`main` (hence destroyed after every function-local static first used by the
test) keeps a thread copying the error after `main` returns, then starts one
more thread from its destructor. The executable replaces `operator delete`
with a version that scribbles freed blocks, so a use after free is detected
deterministically without ASan (the old implementation fails it; ASan reports
it as a heap use after free). On Linux TSan/LSan builds, whose runtimes define
strong allocation symbols, the replacement is left out and the sanitizer does
the detection.

## `thread_pool`

```cpp
class thread_pool final : public executor {
public:
    explicit thread_pool(std::size_t threads = std::thread::hardware_concurrency());
    ~thread_pool();                       // shutdown()
    void post(work_item& item) noexcept override;
    bool running_in_this_thread() const noexcept override;
    std::size_t size() const noexcept;
    void wait_idle();
    void shutdown() noexcept;
};
```

### Queues

- **Local rings.** Each worker owns a bounded ring of 256 `work_item*` slots with
  64-bit `head` and `tail` indices on separate cache lines. Only the owner pushes:
  it stores the slot and then publishes `tail` (release), with no read-modify-write
  and no lock. Consumers (the owner popping its own oldest item, and thieves)
  read the slots `[h, h + n)` and claim them with one `CAS(head, h, h + n)`. The
  owner overwrites a slot only when the ring has room relative to the current
  `head`, so a successful CAS proves that the slots read were not overwritten; a
  stale reader's CAS fails and retries. Indices never wrap (no ABA). Slots are
  relaxed atomics, so the speculative reads are not data races. The owner runs
  its own posts in FIFO order.
- **Overflow.** When the ring is full the owner claims its older half with the
  same CAS and moves it, linked through the items' `next` fields, to the injection
  queue, where every worker (the owner included) can take it. No allocation.
- **Injection queue.** Posts from other threads append to one shared intrusive
  FIFO (spin lock). A worker whose ring is empty takes the oldest injected item
  plus a batch (up to `size / workers`, at most 32, bounded by the room in its
  ring), so outside posters and workers rarely meet on that lock.
- **Stealing.** A worker with nothing local and nothing injected visits the other
  workers in a pseudo-random rotation and takes the older half of a victim's ring
  (at most 32 items), running the first and keeping the rest. A victim with a
  *single* queued item is usually about to pop it (it is the continuation it just
  posted); taking it would only bounce work, and the victim would steal it back.
  A thief therefore takes a single item only if the victim's completion counter
  has not moved since the thief's previous look, i.e. the victim is stuck in a
  long item (the idea of Go's delayed `runnext` stealing).
- **Fairness.** Every 61st pop a worker looks at the injection queue before its
  own ring, so a worker whose items keep re-posting cannot starve outside work.
- Items are taken before they run and are never touched afterwards; no lock is
  held while an item runs. Nothing is allocated per post: the workers, their
  rings and the sleeper stack are allocated once in the constructor.

Worker fields are padded to 128 bytes (the cache-line pair on Apple silicon, two
lines on x86-64): the consumer index, the owner-written fields, the sleep state and
the slots each start a new line, and no two workers share a line.

### Sleeping and waking

One atomic word `idle_` packs two counters: *searching* workers (spinning on the
queues) and *sleeping* workers. The sleeping workers sit on a small stack under a
`std::mutex`; each blocks on its own `std::atomic<std::uint32_t>` with C++20
`wait`/`notify_one` (a futex on Linux, `__ulock` on macOS).

- **post** (after publishing the item): load `idle_`; wake one sleeper only if
  `sleeping > 0 && searching == 0` and there is work to spare: always for an
  outside post, and for a worker's post only if its ring then holds at least two
  items. A worker's single queued item is its own next item; it runs as soon as
  the current item returns, and waking someone for it would only cost a system
  call (and, with stealing, bounce the work). A worker that takes a batch from the
  injection queue or a victim, or overflows its ring, applies the same check.
  When every worker is busy (`sleeping == 0`), or a searcher will find the item,
  there is no system call.
- **A worker without work** becomes a searcher (if fewer than half of the workers
  are already searching, a cap that bounds contention on the queues), spins over
  its ring, the injection queue and the victims with exponential `cpu_relax`
  back-off and then `yield`, for a bounded number of rounds.
- **Found work while searching**: decrement `searching`. If it was the *last*
  searcher and someone sleeps, wake one: that worker becomes the next searcher,
  so a burst of work ramps up the number of active workers one by one.
- **Going to sleep**: under the sleeper mutex push itself, set its park word, and
  move itself from searching to sleeping in `idle_`; then *scan again*, taking
  the injection queue's lock regardless of its size hint. If something is found it
  undoes the sleep (or, if a waker already claimed it, continues as a searcher
  that found work). Otherwise it waits on its park word.
- **wake_one**: under the mutex pop a sleeper, move it from sleeping to searching
  in `idle_`, clear its park word; notify after unlocking.

**No lost work.** An item in a worker's ring always has an awake owner: only the
owner adds to its ring, from inside an item it is running, and a worker goes to
sleep (or exits) only after finding its ring empty. So the owner runs the item
when its current item returns, even if nobody steals it; wake-ups for ring items
only add parallelism. (Hence an item must not block waiting for work it posted to
the same pool, the usual executor rule.) Overflowed items enter the injection
queue, which is covered next.

**No lost wake-up for injected items.** Take an item injected by a post whose
`idle_` load returned `v` and suppressed the wake. Either `sleeping == 0` or
`searching > 0` in `v`. Consider any worker whose transition (start searching,
stop searching, go to sleep) comes *after* `v` in the modification order of
`idle_`. Each of those transitions is followed by a scan of the injection queue
under its lock (the searcher's spin scans, the sleeper's locked rescan) or by a
wake whose target scans. If that scan's critical section came before the post's,
the lock's release/acquire chain would make the worker's `idle_` update happen
before the post's load, so the load would have returned that update or a later
value, contradicting its position after `v`. Hence the scan comes after the post
and sees the item. Such a transition always exists while the item is queued: a
counted searcher must eventually stop searching (finding work or sleeping), and
if every worker is busy each of them will start searching again. (A searcher's
scan reads the injection queue's size hint first; it can skip a just-injected
item, but its final rescan before sleeping takes the lock.) The argument uses
only the lock's acquire/release and acquire/release RMWs on `idle_`; no
sequentially consistent fences are needed (and none would be TSan-friendly on
GCC).

### `running_in_this_thread`, `wait_idle`

A `thread_local` pointer names the worker running on the current thread (and its
pool). It makes `running_in_this_thread()` exact, and routes posts from workers to
their local queues.

Each worker has two single-writer counters, *posted locally* and *completed*
(incremented after an item's `run` returns), and the injection queue counts
outside posts. A post is counted (release store) *before* the item is published:
otherwise a thief could take the item, run it and count its completion before
the post was counted, and `wait_idle()` could see equal sums while other work was
outstanding (found by a fan-out test, now `regression_wait_idle_counts_stolen_items`).
`wait_idle()` reads every *completed* counter (acquire) and then every *posted*
counter. Because an item's post happens before its completion, every counted
completion has its post counted, so equal sums mean every counted
item has finished; anything posted before `wait_idle()` was called, or by a
counted item, is counted. If the sums differ it waits on a condition variable
under the sleeper mutex; a worker that goes to sleep notifies it when waiters are
registered. Since the check and the registration are under that mutex, and the
last worker to finish always goes through the sleep path afterwards, the waiter
cannot miss the idle moment. `wait_idle()` from a pool thread asserts in Debug
(it would wait for itself).

### Shutdown

`shutdown()` (also called by the destructor) is idempotent and serialised by a
mutex, so a concurrent second call waits for the first.

1. Under the sleeper mutex set `stopping`, pop every sleeper and clear its park
   word; then notify them.
2. A worker that finds no work and sees `stopping` performs one last scan of its
   ring, the injection queue (under its lock) and the other rings; if it is all
   empty it exits. Every item posted before `shutdown()` is visible to that scan,
   and items posted *by running items* go to the poster's own ring (or, on
   overflow, to the injection queue), which that worker drains before it can
   exit. So everything already posted runs, including items
   posted by those items while shutting down.
3. Join the workers.

Posting from outside the pool after `shutdown()` has started is a precondition
violation (Debug asserts). In Release the item is still enqueued: it runs if a
worker has not exited yet, and otherwise it is never run. Destroying the pool
from one of its own threads (it would join itself) asserts in Debug. A machine
bound to the pool that is still waiting for a completion from another thread
(a timer, an I/O thread) must finish before the pool is shut down: that
completion would be such a late outside post.

### Machines bound to the pool

Nothing changes in the engine: `machine::dispatch` posts to the home executor
when a completion arrives on a thread for which `running_in_this_thread()` is
false, and continues inline otherwise. A completion delivered by another pool
worker therefore continues on that worker; one delivered by a timer or foreign
thread is posted to the injection queue. `defer()` always posts, so from a
worker it lands on that worker's ring (and may be stolen).

**Engine fix found here (M1 amendment, see `docs/M1-concurrency.md`).** A
`defer()` issued off the pool (for example from `begin()` on the thread that
started a bound machine) posted while still owning the machine; a worker that ran
the item before the driver suspended only signalled readiness, and the step
continued on the starting thread. `test_scenarios` caught it under load (a step
observed `running_in_this_thread() == false`). Off the executor the driver now
hands ownership over before posting.

### Measured behaviour (see `.claude/plans/report-M4.md`)

Each worker running its own chain of re-posting items costs about 4–5 ns per item
on each worker, flat from 1 to 8 workers. A single worker fanning out many tiny
items is bounded by that one producer: thieves add parallelism only when the
items do real work, and a `when_all` of trivial machines additionally contends on
the combinator's shared counter and on its cancellation source.

## `testing::virtual_executor`

A single-threaded executor *and* timer service on virtual time, for reproducible
concurrency tests.

- **Ready set.** Posted items form an intrusive FIFO; timers whose deadline has
  been reached form a second intrusive FIFO of due entries (the entry's
  implementation-owned links are reused; a sentinel in `child` marks "due"). Both
  are guarded by one spin lock, so `post`, `schedule` and `cancel` are
  thread-safe; items and timer callbacks run only inside `run_*` on the calling
  thread, outside the lock. Nothing is allocated, ever.
- **Virtual time.** `now()` starts at `clock::time_point{}`. Time moves only in
  `run_for` and `run_until_quiescent`, and only when no item is ready: then it
  jumps to the earliest pending deadline and every entry due at that instant
  becomes ready (in deadline, then scheduling order). A deadline at or before
  `now()` is ready immediately, so `run_until_idle()` runs it.
- **`schedule::fifo`.** One global FIFO across posts and timers: each due timer is
  stamped with the number of items posted before it became due, and the head item
  runs before the head timer exactly when it was posted earlier.
- **`schedule::random`.** The next item is drawn uniformly from the whole ready
  set (posted items and due timers) with a splitmix64 generator seeded by the
  constructor. Selection walks the intrusive list (O(ready)), which is fine for
  test-sized ready sets and keeps the executor allocation-free.
- **Determinism.** The generator is the only source of choice, it is advanced once
  per choice among two or more candidates, and every structure is ordered by
  program actions only (no addresses, no hash order, no wall clock). Two runs of
  the same single-threaded program with the same seed therefore execute the same
  sequence. Posts from other threads make the order depend on their timing, as
  the documentation says.
- **Cancellation.** `cancel()` removes a scheduled or due entry (returns true),
  returns false when the callback has run, and waits when the callback is
  currently running on another thread (a stack-resident firing record, as in
  M2's timer queue); from inside the callback it returns false without waiting.
- `running_in_this_thread()` is true only inside `run_*` on the running thread
  (a `thread_local` scope chain, as in `manual_executor`), so machines bound to
  the executor continue inline when the executor delivers their completions and
  are posted when a completion arrives from elsewhere.
- `run_*` must not be called concurrently from several threads; nested calls
  from inside an item are allowed.

## Live task registry (`debug/live_tasks.hpp`)

**Opt-in, whole program.** The CMake option `STEPWRIGHT_TRACK_TASKS` (default OFF)
adds `STEPWRIGHT_TRACK_TASKS=1` to the `stepwright` interface target, so every
consumer translation unit agrees. `config.hpp` defaults the macro to 0.
**Tracking changes the layout of `machine<D>`, `source_machine<D, T>` and
`step`/`source_step`**, so it must be set consistently for the whole program;
mixing tracked and untracked translation units that share a machine type is an
ODR violation. With tracking off nothing is added: no member and no call (the
engine's hooks are empty inline functions; only the tiny `detail/live_state.hpp`
enum header is always included).

**Node.** Tracked machines embed a `detail::live_node`: intrusive list links, the
compile-time type name, the start time and starting thread (written before
linking, under the registry lock), and two relaxed atomics updated by the
driver: the state (`running`, `waiting_child`, `parked`, `deferred`,
`suspended`) and the name of the step the machine will resume at. Step names
come from the step's member-function-pointer template argument at compile time
(`detail::value_name<Fn>()`, the same `__PRETTY_FUNCTION__` technique as
`type_name`), so a transition carries one more pointer when tracking is on.

**Lifecycle.** A `machine` registers in `on_start` and unregisters in `finish`
just before the hooks and the receiver (which may destroy it). A
`source_machine` registers when its driver first runs (the first pull or
readiness check) and unregisters when it ends or fails, or in its destructor if
it was destroyed between pulls. Registration links the node into a global
doubly linked list under a `std::mutex`: no allocation, O(1). The registry
itself is immortal (placement-constructed, never destroyed) for the same reason
as `cancelled_error()`.

**Inspection.** `debug::for_each_live(f)` copies the entries into a snapshot
under the lock, unlocks, and then calls `f(const live_task&)` for each, so no
user code runs under the lock and `f` may itself start or finish machines.
`debug::dump(os)` prints one line per entry:

```
<type> state=<state> step=<resume step or -> age=<microseconds>us thread=<id>
```

The header is not part of the umbrella `stepwright.hpp` because it pulls
`<ostream>` and `<vector>` into every user; it compiles with
tracking off too (it then reports no entries), so diagnostics code needs no
`#if`. `thread_pool.hpp` and `testing/virtual_executor.hpp` are in the
umbrella (the latter, like `manual_timer_service`, has no threads and no cost
unless used).

`test_live_tasks` enables tracking for its own executable only
(`target_compile_definitions(... STEPWRIGHT_TRACK_TASKS=1)`). That is safe: the
program is that one executable, all of its stepwright translation units see the
same definition, and nothing is shared with other test executables. A full
build with `-DSTEPWRIGHT_TRACK_TASKS=ON` runs the whole suite tracked as well.

## Stress, scenarios and timing

- `test_thread_pool`: one million items (outside and nested posts) each run exactly
  once; repeated fan-outs whose items are stolen, checked after every
  `wait_idle()`; eight posting threads; shutdown with pending and nested items; idle CPU
  time measured with `getrusage` over 200 ms (workers must be asleep) plus wake-up
  latency after idleness; bound machines continue on the pool.
- `test_virtual_executor`: FIFO and seeded random order (reproducible, differs by
  seed), virtual-time `delay`, `with_deadline`, `interval`, the merge
  head-of-line case, quiescence, and a 1,000-seed cancellation race sweep that
  checks invariants and reports the failing seed.
- `test_scenarios`: fan-out/fan-in of 10,000 machines, a producer → bounded
  channel → merge → `for_each` pipeline, 64 machines contending on an
  `async_mutex`, and `with_deadline` with random cancellation on a steady timer,
  all on a pool with machines bound to it.
- `test_chaos` runs its units a second time with every machine bound to a pool.
- `STEPWRIGHT_STRESS_SCALE` multiplies the iteration counts as before.
