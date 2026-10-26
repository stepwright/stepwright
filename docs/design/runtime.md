# Runtime: pool liveness, virtual execution and diagnostics

## Thread pool queues

Each worker owns a bounded ring of 256 intrusive `work_item*` slots. Its 64-bit
consumer head and owner-written tail are separate cache lines. The owner stores
a slot, then release-publishes tail. Owner and thieves read slots speculatively,
then claim `[head, head+n)` by acquire/release CAS on head. Slots are relaxed
atomics: losing speculative reads are not data races. The owner may reuse a slot
only after acquiring a head that proves its prior consumer finished reading it.
Indices are assumed not to wrap during the pool's lifetime.

The owner keeps `head_seen`, an acquired lower bound on head, refreshed when the
ring appears full or its own pop succeeds. Conservative room checks avoid reading
the thieves' cache line on each push. A full ring moves its older half to the
locked injection FIFO without allocating. Outside posts append to that FIFO.
Workers take a first item plus up to 32 extras (bounded by available room and
queue size / workers) into their rings.

Stealing visits victims in a pseudo-random rotation and claims their older half,
at most 32 items. Ordinary searches delay a single-item steal until a second look
finds no victim completion progress, avoiding needless bouncing of local
continuations. Complete scans before sleeping, and guard scans, steal single
items immediately. Every 61st local pop checks injection first to avoid starving
outside work. Items are removed before invocation and never touched afterwards.

Worker state is padded to 128-byte units (the Apple silicon cache-line pair and
two x86-64 lines). Tail/completed share one polled line; posted and other
owner-private state are separate. All rings, worker records and sleeper slots
allocate once at construction, never per post.

## Sleeping, search hand-over and the guard

`idle_` packs searching and sleeping counts. At most a quarter of workers (at
least one) search at once. A search uses bounded exponential CPU relaxation,
then yields. Sleepers occupy a stack under `sleep_mutex_`. Each worker has an
atomic park word and a condition variable, whose predicate is that park word.
Waking pops one under the mutex, RMW-transfers its count from sleeping to
searching, clears its park word, then notifies outside the mutex. The last
searcher that finds work wakes a replacement before running the item.

**An idle worker must not sleep indefinitely while another worker is running.**
The CAS that publishes its sleeping count also determines whether another worker
is awake and neither searching nor sleeping. If so it waits for at most 250 µs
(subject to OS scheduling) before a complete scan, including eager single-item
stealing. Otherwise it waits indefinitely. The predicate wait releases the mutex;
no internal lock spans sleeping or item execution. A timeout removes its own
sleeper registration under the mutex, then scans unlocked and hands remaining
work over before running anything it finds.

The mode decision must share that sleeping CAS: a stale load of “nobody running”
cannot justify an indefinite wait if another worker took work in between. A
worker that earlier declined to search needs the **same check**: the searcher it
saw may since have become busy. Its own uncounted idle status is excluded when
checking for another running worker. When the last work finishes, timed guards
recheck and become indefinite sleepers; no guard polls a completely idle pool.

Timed guards count as **sleeping**, not searching. Ordinary outside posts and
multi-item local queues can wake them promptly, interrupting their timed wait.
Counting a sleeping guard as a searcher would suppress those ordinary wake-ups,
adding a timer tick of latency to tiny workloads. The condition variable makes
the guard wait interruptible without adding operations to a single local post.

### Liveness of ring items

Suppose a worker posts an item and then blocks forever. While that worker is
running, an idle worker retains a timed guard wait; it eventually scans and steals the item
if some worker is idle. If all workers are busy, an item cannot be promised CPU
time until one becomes available. Search caps do not discard work: a nonsearching
worker can sleep only while another searcher remains, or with an awake owner.

The boundary is atomic. A worker acquiring work from searching becomes running
with an RMW, before executing it. Either that transition precedes another idle
worker's sleeping CAS (which then selects a timed wait), or follows it (the taker
observes the now-last search and hands it to a sleeper before running).
A pre-sleep scan that finds work likewise undoes sleeping and hands the search
over, even if a concurrent waker already claimed that sleeper. The timer-expiry
path also hands over. Count ABA does not introduce a blocked worker: equality
with zero other running workers means the intervening
workers returned to searching/sleeping, after draining their own rings.

This is why a single local post needs no expensive fence or unconditional wake.
Its normal owner will pop it; if the owner blocks, the guard takes it. Larger local
queues prompt an immediate wake for parallel ramp-up when no searcher exists.
Guarding stays in the idle path, outside the hot queue loop. Blocking is still
discouraged: it consumes finite worker capacity,
and a dependency cycle with **every worker blocked** cannot make progress. Prefer
`after`/`park`, which suspend machines without blocking workers.

### Injected and moved items

Outside posting and a sleeper's forced rescan both acquire the injection lock.
If a post suppresses a wake because a searcher exists, that searcher must scan,
hand over the search, or register as asleep and rescan under the lock. If a scan
preceded the post's critical section, its `idle_` transition would happen before
the post's later acquire load; it cannot simultaneously be an unseen future
transition. The final scan ignores the queue's relaxed size hint.

Batch taking and stolen extras move work behind an item that may block. A
nonsearching mover announces with `idle_.fetch_add(0, acq_rel)`, not a load: it
either sees a sleeper's registration or precedes that RMW, whose acquired rescan
then sees the published tail. A searching mover instead relies on its
`stop_searching(true)` RMW and hand-over. A successful sleeper rescan also hands
over; without it, work left to that searcher could strand behind its blocked item.
All these paths retain the original locked-injection no-lost-wake guarantee.

## Idle detection and shutdown

Local posts increment the worker's single-writer posted counter **before**
publishing the item; outside posts are counted under injection's lock. A worker
increments completed after invocation, without touching the item. `wait_idle`
acquire-reads completed counters before posted counters: every counted completion
has its post counted, so equal totals mean all counted work finished. Posts made
before the call and posts made by that work are covered; concurrent outside
posts may or may not be included. A condition variable registration under the
sleeper mutex prevents missing the last worker becoming idle. Calling it on a pool
worker is invalid because it waits for itself.

Shutdown is serialised, idempotent and forbidden on a pool worker. It sets
stopping, releases sleepers, lets workers perform final complete scans and drain
their own rings, then joins. Work posted by running items is drained too. Outside
posts after shutdown begins are invalid; Release may run them only if a worker
has not exited yet. Finish timer/I/O producers and pool-bound machines before
shutdown: a late foreign continuation is such an outside post. Do not rely on
extra workers surviving shutdown to satisfy blocking dependencies.

## Teardown and lifetimes

A delivered completion is a lifetime boundary, not just a state change. After
invoking a receiver, the library never accesses the completing task/waiter again;
after invoking an item or timer callback, it never accesses that intrusive node
again. The observer must synchronise with the completion's effects (for example,
a release store in the receiver and an acquire load by the owner). It may then
destroy the completed object, even before the delivering call returns, subject
to the type's other preconditions. Batched releases require every outstanding
wait to complete; a source's cancelled readiness wait does not settle a suspended
producer. Stop independent wakers and other producers before reclamation: a raw
pointer and a thread-safe member function do not acquire lifetime ownership.

The same rule applies to shared runtime resources: after observing its last
item/callback, an owner on another thread may destroy the pool/timer service even
if the publishing `post`/`schedule` has not returned. These services synchronise
any remaining internal accesses before destroying their members. Destruction from
their own worker/callback is forbidden. No new outside post may begin at or after
shutdown starts; the permitted overlap is the suffix of an already-published
post, not an uncoordinated producer or a later continuation. Settle all scheduled
timers, registered waiters and active operations before reclaiming their owners.

### Outside pool posts

Outside posts append/count their item and consume a queue-protected wake flag
while holding the injection lock. With no wake needed, releasing that lock is
the post's final pool access. Workers must acquire it to dequeue the item, including timed-guard
rescans. Thus every poster access happens-before the unlock, which synchronises
with dequeue, then execution, then the owner's observation and destruction.

A post needing an out-of-lock wake increments `outside_waking_` **before**
releasing injection. After releasing it, the poster wakes a worker and performs
a release decrement as its **last pool access**. No notification follows that
decrement. Shutdown joins workers, then acquire-waits for the counter to become
zero before any members can be destroyed. The increment happens-before dequeue,
execution and the owner's observation, so the destroying thread cannot miss the
pin by reading the initial zero. Joining workers likewise orders publication of
the items drained during shutdown. No new outside posts are permitted, so zero
means every pinned suffix has ended. Atomic RMW release sequences make the final
acquire synchronise with every poster's release, even with out-of-order returns.
All suffix accesses therefore happen-before member destruction. The counter is
assumed not to overflow (fewer than 2^64 simultaneous waking posts).

The pin is deliberately limited to posts that need wake-up work. Ordinary posts
have no extra RMW, lock or allocation, and worker-local posts never use it. The
counter occupies a separate cache line. Shutdown yields while waiting, without a
per-post notification that could itself race reclamation. Merely moving notify
under the sleeper mutex would not suffice: the idle load and mutex acquisition
also need to precede publication or be protected. Holding injection through a
wake is safe but needlessly blocks takers; pinning the rare wake suffix keeps
the injection critical section short, without nested injection/sleeper locks.

`inject_needs_wake_` starts **true**. Every forced injection scan that finds no
item rearms it under the injection lock if the quarter-of-workers search budget
has room (`4 * searching < count_`, the same budget as `start_searching`). Such a
scan is mandatory between
sleep registration and actually waiting; timed-guard and final shutdown scans
use it too. The first outside post consumes true, pins the pool and promises a
wake. Subsequent posts see false and coalesce behind that wake/search obligation,
without reading the contended shared idle word on the posting path.

This preserves the no-lost-wake edge directly on the queue lock. If the forced
empty scan precedes publication, the post sees its flag and wakes; if publication
precedes the scan, the scan takes the item. A previous post that consumed the
flag either still owes its pinned wake, or has handed the search to a worker.
Before that worker can sleep after draining work, its own mandatory empty rescan
rearms the flag, or finds the search budget already full. In the latter case at
least one other searcher remains responsible: before the last searcher sleeps,
its own sleep-registration RMW publishes zero searching and its locked empty
rescan rearms the flag. The queue lock orders a previous such rescan before a
post, or makes the rescan take that post's item. Existing batch/steal search
hand-over and timed guards cover a
taker that blocks in user work, including work moved to a local ring. A wake that
finds no sleeper is harmless: an awake worker will scan, or its future sleep
registration is followed by the locked rescan. Other forced misses may rearm the
flag conservatively while a worker searches; at most one subsequent post consumes
each such rearming, so extra wakes are bounded by scans, not by the posting rate.

There is no new polling, timed wait or system call in the idle path, and the guard
period and sleep-mode selection are unchanged. An already-required empty locked
scan just stores one flag in the queue's metadata line. Completely idle pools
still sleep indefinitely with zero recurring work. Worker-local ring operations
and overflow/movement wake-ups are unchanged; workers themselves are protected
by shutdown's join. `wait_idle()` may
return before a pinned suffix ends; destruction and explicit `shutdown()` wait
for it. An already-claimed/expired sleeper can make `wake_one` a no-op; its pin
is still released on that path.

The foreign single-item injection helper is separate from worker batch overflow.
Neither it nor `post()` is forced inline: forcing them into callers' posting
loops made one-worker outside posting about 25% slower on AppleClang. On
AppleClang the persistent worker loop is kept out of the thread-start closure
and aligned, avoiding a measured hot-loop code-generation regression when the
foreign branch grew. These are code-generation hints only:
they add no worker-local atomics or allocations and do not change dispatch,
counter ordering, guard behaviour or class layout between Debug and Release.

Outside posted-count stores are relaxed under injection locking. This does not
weaken `wait_idle`: the store precedes injection's release unlock, the worker's
acquire dequeue, and its release completion counter. `wait_idle` acquire-reads
completions **before** reading posted counts, so any completion it counts makes
its post count happen-before that later load; atomic coherence cannot return a
count predating the acquired completion's post. Earlier calls ordered by the
caller are likewise visible; concurrent outside calls may be included or not as
documented. The injection unlock already publishes both node and count, so a
second release store on the count itself would be redundant. Local count and
ring publication ordering, and the lifetime pin's release/acquire ordering, are
unchanged.

Concurrent callers of `wait_idle`/`shutdown` must still
return before another caller destroys the pool: those calls are not items and do
not hold automatic lifetime ownership.

### Steady timers

`schedule` inserts and, if necessary, notifies **under the same mutex**. Its final
service access is the mutex unlock. The worker must acquire that mutex before
removing the entry, so that unlock happens-before the callback and its observer.
There can be no outstanding scheduler notification when destruction is justified
by that callback. Notifying a condition variable under its predicate mutex is not
invoking user code under a lock: callbacks still run unlocked.

`cancel` removes/rechecks entries under the mutex and returns via a final unlock;
foreign callbacks are waited for. The worker uses a stack firing record and never
touches the entry after invoking it. Its later record cleanup and waiter
notification are safe because the destructor joins the worker before destroying
the queue, mutex or condition variables. The destructor's own stop notification
precedes that join. Other concurrent cancellations must return before service
destruction; observing the callback is not a join of unrelated API callers.

### Publish, then arm cancellation

`wait_core::prepare` leaves `armed` clear for a cancellable token. A resolver may
publish its result meanwhile, but cannot deliver the receiver until `arm` sets
`armed` after binding and activating cancellation. The atomic phase handshake
therefore keeps a cancellable wait alive through arming; setting `armed` (or the
resulting finish) is the starter's final object access.

For a token that cannot cancel, `prepare` sets `armed` **before publication**.
The wait may then complete, and its owner may destroy it, before publication
returns. Every starter must test its parameter or a local owning token copy and
**skip `arm` entirely** for that token. Reading a token back from the object, or
calling even a no-op member on it, is already too late. This applies after timer
scheduling (`delay`, interval), waiter-list unlocks (event, both mutexes, channel
send/readiness, broadcast publish/subscription), shared-task publication/inner
start, and the source-machine readiness CAS.

Merge already tests a local `cancellable` value after releasing its starter unit:
an empty-token consumer can have completed at that release, while a cancellable
one is still protected by the phase handshake. Joins arm the parent link before
starting children, and deadlines retain a starter unit through timer publication,
parent linking and inner start. Their final unit release is their last access,
even with an empty parent token; these unit barriers must not be removed. The
other source-machine arming site, `set_token`, is configuration while not
suspended, not publication of a readiness wait. It must still replace/disarm the
previous registration when given an empty token.

The teardown regressions force delay and interval callbacks on another thread
before `schedule` returns and destroy the operation inside completion. They check
both empty tokens (delivery before return) and cancellable tokens (delivery held
until arming). Linux ASan/UBSan catches the invalid post-lifetime member call in
the unfixed delay even though the old empty-token `arm` body did no work.

### Queries and caller-driven runtimes

Queries are snapshots, **not destruction signals**. In particular `event::is_set`
and `shared_task::is_done` can observe publication before waiter detachment or
delivery finishes. Channel/broadcast `is_closed`, channel `size`, subscription
counts and executor/timer pending counts do not prove notifications or callbacks
have finished. A mutex-protected query only orders the protected state, not work
done after unlocking. Cancellation's requested flag does not prove callback
dispatch has finished; live-registry absence does not prove a receiver has run.
Immutable size/capacity/home/seed/token metadata likewise grants no lifetime
ownership. Use completions or externally synchronise the changing calls instead;
this keeps these queries cheap and preserves their existing snapshot semantics.

Caller-driven runtimes have an additional lifetime precondition: keep a manual
executor alive through `run_all`, a manual timer service through `advance*`, and a
virtual executor through every `run_*` call. These loops must revisit their state
after a callback and have no background thread for their destructors to join.
They may deliver a callback that destroys its **node**, not their service. Manual
timer operations are actually thread-safe (including foreign cancellation), while
virtual execution is single-driver, with same-thread nesting supported; neither
property permits destruction during a driving call. `post`/`schedule` publication
in these runtimes ends with the queue-lock unlock, before another driver can take
the node. Tests exercise concurrent publication and node reclamation, keeping
caller-driven services alive until their driving calls return.

`test_teardown` exercises heap reclamation on a different thread **before** joining
the producer, including pool rescans, due timers, waiter batches, cancellation,
streams, joins, deferrals and registry snapshots. Joining first would hide a
missing happens-before edge. Linux ThreadSanitizer detects the original
notification/destruction races; macOS sanitizers alone are not sufficient.

## Deterministic virtual executor

`testing::virtual_executor` is both an executor and timer service. Posts and due
timers form intrusive ready FIFOs under a spin lock; callbacks run only inside
`run_*`, on its caller, outside that lock. Time starts at the steady clock's zero
time point. `run_until_idle` does not advance it. `run_for` and
`run_until_quiescent` jump to the earliest deadline only when nothing is ready.

FIFO mode interleaves posts with due timers stamped with the count of earlier
posts. Random mode uses seeded splitmix64 to select among all ready candidates;
intrusive list traversal is O(ready), appropriate for tests and allocation-free.
The same single-threaded program and seed reproduce execution order. Foreign
posting introduces real scheduling nondeterminism. Timer cancellation uses stack
firing records and waits for foreign callbacks, like the other timer services.
`run_*` calls are not concurrent, though same-thread nesting is supported.

## Live registry and static lifetime

Tracking is whole-program opt-in via `STEPWRIGHT_TRACK_TASKS=1`; it changes machine,
source-machine and transition layout, so mixing definitions violates the ODR.
With tracking off there is no node or hook cost. Tracked machines embed an
intrusive node registered under a mutex from start to completion; source machines
register on first execution until terminal outcome or nonsuspended destruction.
Driver-owned relaxed atomics expose state and resume-step names for diagnostics.
Snapshots copy under the registry lock and invoke user callbacks or print only
after unlocking; snapshots allocate and are not suitable for signal handlers.

Both the registry and shared cancellation error are placement-constructed in
static storage and intentionally never destroyed. Their payloads remain reachable
for leak checking; threads completing during static teardown cannot copy freed
state. Static-lifetime, allocation, tracked-layout, pool-idle CPU, million-item,
bounded liveness, virtual-seed and multi-component scenario tests cover these
contracts. Stress rounds scale with `STEPWRIGHT_STRESS_SCALE`.
