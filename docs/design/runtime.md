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
