# M5 performance: scaling fan-out, and the benchmark suite

Independent design from the M5 specification
(`.claude/plans/spec-M5-examples-performance.md`) on top of M1–M4. No earlier
guarantee is weakened; every M1–M4 test passes unchanged, and the class layout of
every public type except `when_all_range`, `when_any`/`when_all` (one pointer
more, see below) and the cancellation state is unchanged (`machine<D>` stays at
112 bytes).

## A1. Why `when_all` of trivial machines did not scale

`BM_pool_fan_out_fan_in/10000` starts a `when_all_range` of 10,000 machines bound
to an 8-worker pool, from a worker. Each child defers once (one pool post) and
completes. Measured on the M4 tree (Apple M4 Max, Release): 368 ns per machine on
8 workers against 32 ns on one.

Instrumenting the run (time spent in the starting worker's loop over the children
versus the tail after it) and sampling it showed that **the starting worker is the
serial bottleneck**: it starts and posts every child, and with 8 workers each
start cost about 75 ns instead of 9 ns, because almost every operation on its path
missed in the cache on a line that the other workers were writing:

1. **Cancellation registration.** Every child machine armed a registration on the
   combinator's internal token in `start()` and disarmed it in `finish()`. Both took
   the token state's single `std::mutex` (a kernel wait on macOS when contended),
   and the state's reference count, request flag, mutex and list head shared one
   cache line, also written by every token copy. Disabling child registration
   (`when_all_policy::run_all` without a parent token, which hands children an
   empty token) cut the cost to 128 ns: the registration was the largest single
   cost, but not the only one.
2. **The combinator's countdown.** Every child completion did one
   `fetch_sub` on one counter, from all workers.
3. **The pool's ring.** Each push read the ring's `head` (CAS'd by every thief) and
   wrote `tail` on the line that also held the `completed` counter, both polled by
   every searching worker on every look; with up to four searching workers (half
   of eight) the lines never stayed with their owner.

## A1. Design

### Machines register for cancellation only while parked

A machine's registration exists to run its cancellation hook. The *default* hook
(no `on_cancel()`) does only one thing: if the machine is parked, wake it with
`errc::cancelled`. A machine that is running a step, waiting for a child or queued
after a deferral ignores the request (its children see the same token). So the
registration is useless until the first park.

`machine<D>` now **binds** the start token to its registration at start (the token
is stored, nothing shared is touched) and **activates** the registration on the
first park of the run (`registered_`, a byte in existing padding, records it);
`finish()` disarms only an activated registration. Machines with an `on_cancel()`
hook activate at start, as before (the hook may run at any time, including before
`begin()` for an already-requested token). A machine that never parks, like every
child of the benchmark, touches no shared cancellation state at all; its token
copy is the only shared write, and that line is now private to the reference
count (below).

**The M1 park/cancel ordering argument still holds.** Park entry is now:

1. activate the registration, if not yet active (link it under its stripe lock,
   or run the default hook inline if the token is already requested);
2. publish `parked` (SC `fetch_or`);
3. query the token (SC load) and, if requested, `wake_if_parked(cancelled)`.

Steps 2 and 3 are the M1 handshake unchanged. Activation happens *before* step 2,
so from step 2 on, a request either finds the registration (and the default hook's
SC load of the state is ordered against step 2 by the M1 argument) or happened
before the registration was linked, in which case step 1 ran the hook inline and
step 3 sees the request (the request's SC exchange precedes the activation's flag
check, which precedes step 3). A hook that runs inline at step 1 sees no `parked`
and does nothing, exactly as the M1 hook did when armed at start and requested
before the park. Wake acceptance is still decided by the conditional CAS, so a
hook never injects cancellation into a child wait or a deferral. A re-park in the
same run reuses the active registration. Regression tests:
`cancel_scaling.regression_lazy_machine_registration` (a park after a child and a
deferral, a second park in the same run, a third run with a new token, a machine
that never parks under a requested token) and
`cancel_scaling.regression_shared_token_parks_race_request` (256 pool-bound
machines park on one token while it is requested; all complete cancelled).

`wait_core` (every M2/M3 waiter) and the machine use `bind` + `activate` instead of
`arm`: their registration is always idle at that point (the previous run's finish
disarmed it), so the redundant disarm (a lock on the old token's state) is gone.
Waiters already registered only while waiting, so nothing else changes for them.

### Cancellation state: striped, with the reference count apart

`detail::cancel_state` is still one allocation per `cancel_source`, now laid out
as (128-byte units, `detail::cache_line`):

- the reference count on its own line (every token copy writes it);
- the request flag and the waiter bookkeeping (below) on the next line;
- 8 **stripes**, each a line with a spin lock, an `occupied` flag and the head of
  an intrusive doubly linked list.

A registration lives in the stripe chosen by Fibonacci hashing of its own
address, so it always finds its stripe again and no field is added to the node
(`cancel_registration` is still 40 bytes). Threads arming and disarming different
registrations on one token mostly take different locks on different lines, and
the locks are short spin locks (the critical sections are a few pointer writes)
instead of a `std::mutex`.

The guarantees, one by one:

- **O(1) deregistration.** Unlinking is the M1 predecessor-pointer unlink inside
  one stripe; the stripe is recomputed from the address.
- **Callbacks run exactly once, outside every lock.** A requester sets the flag
  (SC exchange, first request only), then visits the stripes. For each stripe it
  skips if `occupied` reads false (SC load), otherwise it locks and, while the list
  is non-empty, unlinks the head, publishes the stack invocation record, unlocks,
  runs the callback and relocks (the M1 procedure, per stripe). Activation links the
  node and sets `occupied` (SC store) under the stripe lock, *then* loads the flag
  (SC). If the flag is set, it unlinks the node again and runs the callback inline.
  Exactly one side runs the callback: both decide under the stripe lock, and the
  side that unlinks the node runs it. At least one side does: if the requester's
  load of `occupied` read false (or an older value) it precedes the activation's SC
  store in the single total order of SC operations, so the request's exchange
  precedes the activation's flag load, which therefore sees the request (Dekker's
  pattern). If the requester visits the stripe after the activation, the lock
  orders them and it finds the node. `occupied` is cleared (SC) when an unlink
  empties a stripe; a stale `true` only costs a lock, and a `false` read while a
  node is linked is exactly the case just covered. Skipping empty stripes keeps a
  request on a token with few registrations as cheap as before (8 SC loads instead
  of 8 lock round trips; `BM_when_any_sync/10`, whose winner always requests
  cancellation, is about 10 % faster than M4).
- **Self-deregistration** is unchanged: a disarm on the invoking thread clears the
  invocation record and returns; the requester touches the node afterwards only if
  the record still names it.
- **Waiting for a running callback.** A foreign disarm that finds the node being
  invoked increments `waiters` and reads `finished` under the stripe lock, unlocks
  and blocks with C++20 `atomic::wait` on `finished`. After a callback returns the
  requester relocks, clears the record and, if `waiters` is non-zero, increments
  `finished` (still under the lock) and calls `notify_all` after unlocking. Because
  both sides access `waiters`/`finished` under the same stripe lock, either the
  waiter's critical section comes first (the requester sees it and changes
  `finished`, so the wait returns) or the requester's does (the waiter sees no
  record and does not wait). Spurious wake-ups (another stripe's callback) only
  loop. The state is kept alive by the requester's local token during the notify,
  and the waiter holds a token through its node. No lock is held while waiting,
  so this cannot deadlock (same argument as M1/M2).
- **Zero allocation.** Nothing is allocated per arm, disarm or request; the
  stripes are part of the one state allocation (1,280 bytes instead of about 150).
- **One-way cancellation, `reset`.** Unchanged: `reset()` (library-internal)
  clears the flag; in Debug it asserts, stripe by stripe, that no registration is
  linked.

`arm()` keeps its exact M1 contract (disarm, bind, activate). The token copy that
previously kept the state alive across every activation is now taken only on the
inline path, where the callback may destroy the node.

Tests (`tests/test_cancel_scaling.cpp`, all also run under TSan with
`STEPWRIGHT_STRESS_SCALE=5`): eight threads arming and disarming on one token followed
by a request (exactly the armed ones run, once); a request racing eight threads'
arms and disarms at a different point in every round (at most once, never after a
disarm returned, exactly once when left armed); callbacks that disarm, destroy or
rearm themselves while other threads churn registrations on the same stripes;
foreign disarms racing callbacks that run for a while (a returned disarm never
sees a callback in progress); and the two machine tests above. The M1 tests in
`test_cancel.cpp` pass unchanged.

### `when_all_range` counts completions in shards

A `when_all_range` of 64 or more children allocates, at construction, 16 counters
on separate cache lines; child *i* completes into counter *i* mod 16, and a counter
that reaches zero releases one unit of the M2 unit count. Children that complete at
the same time on different workers (they were posted consecutively) therefore
update different lines, and the shared unit count is touched 16 times per run
instead of 10,000. A successful child touches only its counter (the owner pointer
is read by the counter's last child only, so the line is not pulled in shared state
first). The M2 protocol is otherwise unchanged: a failing child records the first
failure and requests cancellation *before* releasing its share, so a unit is still
held across `request_cancel()`. Memory ordering: each counter's `fetch_sub` is
acq_rel, so every child's writes happen before the counter's last decrement, which
happens before the unit release that completes the combinator. `when_all<N>` (a
fixed, usually small N) and `when_any` keep the single counter. Construction of a
large `when_all_range` now allocates twice (internal cancel source and counters);
runs still allocate nothing (`alloc_m2.regression_zero_alloc_sharded_when_all_range`).
Tested by the existing 64-child many-thread test (now sharded),
`when_all.regression_sharded_completion_on_a_pool` (64, 65 and 1,000 pool-bound
children; failures in different shards, both policies, a parent cancelled mid-run,
restart; the combinator never completes before the last child) and the 10,000-child
tests.

### Thread pool: fewer polled lines, fewer pollers

- A worker's `tail` and `completed` (read by thieves on every look) share one line;
  `posted` (read only by `wait_idle`) moved to the owner-private line.
- The owner keeps `head_seen`, a lower bound of its ring's `head`: a push rereads
  `head` only when the ring looks full, and the owner's own pops refresh it. Since
  `head` only grows, the room check stays conservative, and every claim up to
  `head_seen` was observed with acquire ordering (the reread, or the pop's CAS), so
  the overwritten slots are no longer being read. A push's queued-item count, used
  only to decide whether a wake is worth it, may now be over-estimated after thefts;
  wakes are hints for ring items (no-lost-work does not depend on them).
- At most a **quarter** of the workers (at least one) search at once, instead of
  half. Every look at a victim costs the victim a cache-line transfer on its next
  push or pop, and one searcher already ramps up the pool (each searcher that finds
  work hands the search to a sleeper). The no-lost-wake-up argument of M4 does not
  depend on the cap. Measured: fan-out 30 → 25 ns per machine, outside posts to 8
  workers 28 → 21 ns per item, ping-pong and per-worker chains unchanged.
  (Tried and rejected: a cap relative to awake workers, as in Go's scheduler, was
  slightly worse here; skipping the sleeper lock with `try_lock` for hint wakes lost
  parallelism; stealing only from rings with more queued items made no difference.)

### Thread pool: a liveness bug present since M4

Found when `test_alloc_m4` hung once in the final validation runs; the reduced
reproduction (`thread_pool.regression_outside_items_never_wait_behind_a_blocked_worker`:
*n* items posted from outside to an *n*-worker pool, each waiting until all of
them run) hangs within seconds on the M4 pool too, with 2 to 8 workers. M4's
no-lost-wake-up argument covers items while they sit in the injection queue, and
ring items under the rule "an item must not wait for work it posted itself". Two
paths escaped both:

1. **Moved items.** A worker that takes an injection batch, or steals extras, puts
   items it did not post into its own ring, and may then run an item that waits for
   them. Its wake decision was a plain load of `idle_`, which can miss a worker that
   is going to sleep at the same moment while that worker's last scan misses the
   items (store buffering across the ring's `tail` and `idle_`). Now a worker that
   moves items calls `wake_reliably()`, whose read-modify-write on `idle_` either
   follows the sleeper's registration in `idle_`'s modification order (and sees it)
   or is read by it (so the sleeper's scan sees the items). A searcher skips it: its
   own `stop_searching()` read-modify-write hands the search over if it was the
   last searcher, and the others still search (and their own sleep registrations
   are ordered after it).
2. **The search ended inside a rescan.** A searcher that gives up registers as a
   sleeper, which ends its search, and then rescans. When that rescan found an item
   it ran it without handing the search over, so work the other workers had left to
   that searcher (their wake-ups were suppressed because it was searching) stayed
   behind blocked items. That path now hands over like `stop_searching()` does.

In addition the rescan before sleeping takes a victim's *single* queued item at
once: the delayed stealing of single items (M4, from Go) needs two looks, and a
worker about to sleep would otherwise never look a second time. A worker going to
sleep has nothing better to do, so this does not bounce work between busy
workers. The extra read-modify-write costs about 1 ns per machine in
`BM_pool_fan_out_fan_in` (it runs once per moved batch, and never with one worker).

### Results (Apple M4 Max, Release; full tables in `docs/benchmarks.md`)

M4 figures from the committed M4 tree built and run the same way in the same
session; ns per machine (per completion for the last row).

| | M4 macOS | M5 macOS | M4 Linux | M5 Linux |
|---|---:|---:|---:|---:|
| `BM_pool_fan_out_fan_in/10000` (8 workers) | 381 | 25.2 | 228 | 24.5 |
| `BM_pool_fan_out_fan_in_one_thread/10000` | 32.5 | 18.2 | 36.3 | 19.5 |
| ratio 8 workers / one (target ≤ 1.5) | 11.7 | 1.38 | 6.3 | 1.26 |
| `_work` speed-up, 8 workers vs one (target ≥ 5) | – | 8.5 | – | 6.7 |
| `BM_shared_token_complete/100000` | 10.9 | 8.9 | 16.4 | 13.8 |

The one-thread reference got faster too (no registration per child), which makes
the ratio target harder, not easier. What remains at 8 workers is inherent to a
single starting worker posting tiny items: the starter still pays a transfer for
each child's state line, and thieves take items about as fast as it posts them.

## A2. Benchmark suite

See `benchmarks/README.md` for what each benchmark measures and why each baseline
is fair, and `docs/benchmarks.md` for published results. The C++20 coroutine
baseline (`BM_coro_*` in `bench_engine.cpp`) is a minimal lazy task with
symmetric transfer written in the benchmark; it counts frame allocations, so the
tables show when the compiler elided them.
