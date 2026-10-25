# Primitives: structured completion and waiter races

All waiters embed their queue links, completion target, result and cancellation
registration. There is no allocation per successful wait. Short spin locks cover
list/counter/heap updates, never callbacks, child starts or executor posts. The
steady timer service uses a mutex because its worker blocks. A structured task
does not complete while work it started is outstanding.

## Hand-off trampoline

`detail/handoff.hpp` maintains a per-thread intrusive FIFO and an active flag.
An outer hand-off runs the item and drains the queue iteratively. A nested hand-off
appends and returns. `handoff_scope` batches event releases and reader grants,
delivering the whole FIFO batch before work its callbacks cause. Nodes are removed
before invocation and never touched afterwards, so a completion may destroy its
waiter. Immediate success inside `start` calls the receiver directly; a machine's
driver loop absorbs it.

Consequently a completion caused by `set`, `unlock` or similar calls **can run
after that call returns**, when the call is inside another trampoline-delivered
completion. Do not block waiting for callbacks queued behind the current callback
on that same thread. A million event or mutex hand-offs use constant stack.

## Publish, arm, resolve

`detail::wait_core` is shared by task waiters and stream readiness waits. A starter
prepares the receiver and publishes its node before activating cancellation,
because activation may invoke inline and must find the published node. Grants may
race with arming. Two bits in one atomic phase byte decide the sole finisher:

```
starter:  prepare; publish; arm(token); old = fetch_or(armed);    if (old & resolved) finish
resolver: result = r;                   old = fetch_or(resolved); if (old & armed) finish
finish:   disarm cancellation; handoff(deliver)
deliver:  move result; phase = 0 (release); invoke receiver
```

Exactly one RMW observes the other bit. With an empty token, `armed` is set before
publication; the starter never touches the object after publishing. Acquire/release
makes the result visible to either finisher. The party that unlinks a list waiter
under its primitive's lock is its unique resolver. Timers instead race for a
`decided` bit. A losing cancellation callback only returns, so a foreign disarm
cannot wait on work that needs the finisher's lock. Self-disarm never waits.

## Events, sharing and exclusion

| Primitive | Protected state and arbitration |
|---|---|
| `event` | `set` stores its result, publishes `set_` and detaches the FIFO under the lock; releases outside it. Waiters recheck under the lock before linking. Cancellation versus set is decided by unlinking. `reset` needs no concurrent use or registered waiters. |
| `shared_task` | `idle → running → done`. The first waiter creates/reuses the internal source, links, unlocks and starts inner. Cancellation unlinks that waiter only; inner continues. `cancel()` requests inner outside the lock. It must not race `reset`, which could otherwise redirect an old request to the next run. |
| `async_mutex` | Unlock transfers ownership directly to the oldest waiter, never briefly making the mutex free: no barging. Grant versus cancellation is decided by unlinking. A winning grant means the caller owns the lock even if cancellation raced. |
| `async_shared_mutex` | One FIFO of readers and writers; new readers queue once anything is waiting. Admission grants the head writer only when free, otherwise the consecutive head readers. Cancelling a queued writer re-runs admission so readers behind it do not remain needlessly blocked. |

`guarded` tasks acquire, await inner, then release even on inner failure. A failed
lock wait skips the no-argument continuation, so it never starts inner or unlocks
an unowned lock. Ownership is not tied to a thread.

## Fan-out joins

`detail::join_base` serves both `when_all` and `when_any`. Its internal token is
linked to the start token. One 64-bit word packs outstanding **units** in bits
0–39 and `when_any` completions in higher bits. Units cover children, the starter,
the winner while it requests cancellation, and temporarily a parent-cancellation
callback. Releasing the last unit completes the join.

A successful `when_all` child does one decrement. A failing child claims the first
failure, stores its error and requests sibling cancellation (default policy)
before releasing its unit. `when_any` uses one RMW to release a unit and count the
completion; its winner stores the index/result and requests cancellation while
holding the winner unit. Both start every child and wait for every completion.
Zero-child `when_all` succeeds even under a cancelled token; zero-child `when_any`
is a precondition violation (`npos` plus success without checks).

A parent callback temporarily adds a unit. If the old count was zero, it undoes
the increment and returns: completion is already decided and disarm is waiting
for that callback. Otherwise it requests the internal source and releases its
unit. Holding units across requests prevents completion/restart from resetting
state while an old request still traverses it. `run_all` with no parent token
passes empty child tokens. A range join of at least 64 children uses 16 completion
shards; [performance](performance.md) explains the ordering and cost.

## Timers and deadlines

`timer_service::entry` is an intrusive pairing-heap node ordered by deadline and
sequence number. Equal deadlines are FIFO. Insert is O(1); pop/removal is amortised
O(log n), implemented iteratively. Under the service lock a due entry is removed
and a stack firing record installed. The callback runs unlocked and may destroy
or reschedule the entry; afterwards only the record is touched.

`cancel` removes a queued entry (true), waits for a foreign callback and rechecks,
or returns false for an absent entry or its own callback. Manual timers yield
while waiting; steady timers use a condition variable. The steady worker copies
a deadline before `wait_until`: the node may be cancelled and destroyed while
the mutex is released. Destruction requires no scheduled work.

`delay` publishes its timer then arms cancellation. Fire and cancel race for
`decided`; cancellation's winner cancels/waits for the losing fire, then resolves.

`with_deadline` has starter, inner and timer units. One CAS chooses inner, timeout
or parent cancellation. Inner success cancels the timer; timeout cancels inner;
parent cancellation cancels both. A removed timer releases its unit on the
remover's path, otherwise the callback releases it. Losing timer callbacks only
release their unit, so waiting for one cannot form a lock cycle. Completion waits
for all units and returns the chosen outcome, not a loser's later error.

## Reuse, adapters and code generation

Internal cancel sources reset only after all old work and requests have completed.
The public source remains one-way. Retained token copies from completed work see
the reused state; callers must not use such tokens to control a later run.

`make_task` stores a callable implementing the task contract. `finally_task` invokes
nonthrowing cleanup before delivering inner's result. `reschedule` posts an
embedded work item and completes there. It ignores cancellation.

Tiny handle destructors and `task::start` expose their empty paths with a
feature-tested inline hint. This prevents large umbrella-header translation units
from introducing cold cleanup calls into successful per-step paths; it changes
neither semantics nor layout. Allocation, layout, million-hand-off stack and
20,000-round race regressions validate these invariants under sanitizers.
