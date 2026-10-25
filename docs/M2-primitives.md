# M2 primitives: design, ownership and races

Independent design from the M2 specification (`.claude/plans/spec-M2-primitives.md`)
on top of the M1 engine (`docs/M1-concurrency.md`); no M1 guarantee is weakened.
Every primitive follows the same rules:

- Waiter and registration storage is embedded in the waiting object; nothing is
  allocated per operation. Long-lived internal cancel sources are allocated once
  and reused (see *Resettable cancel source*).
- Internal locks are short spin locks (`detail::spin_lock`, test-and-test-and-set
  with yield) around list/counter/heap updates only. No task is started, no
  completion delivered, no executor posted to and no user code run while one is
  held; the steady timer thread uses a `std::mutex` because it must block.
- A component never completes while anything it started is outstanding.
- Layout never depends on `NDEBUG` (checked for all 26 public M2 types by
  `layout.regression_m2_layout_ndebug`).

## Hand-off trampoline (`detail/handoff.hpp`)

A per-thread intrusive FIFO of `work_item`s plus an `active` flag.
`handoff(item)`: if no trampoline is active on this thread, mark it active, run the
item, then drain the queue iteratively; otherwise append the item and return.
The node is the waiter's own embedded `work_item` (its `next` field is owned by
the trampoline while queued), so queueing never allocates. Items are unlinked
before they run, so a completion may destroy its waiter. `handoff_scope` is an
RAII batch: while alive, hand-offs only queue; the outermost scope drains. It is
used where one state change releases many waiters (event set, inner completion
of a shared task, reader admission) so the whole batch is delivered in FIFO order
before anything those completions cause.

Consequence (documented on `set`, `unlock`, ...): a completion caused by a state
change may run *after* the state-changing call returns, when that call was made
from inside another trampoline-delivered completion on the same thread. A chain
such as unlock → next owner runs → unlock → ... therefore uses constant stack:
the 1,000,000-step mutex chain, the 1,000,000-step event ping-pong and a raw
1,000,000 hand-off chain all stay within a 64 KiB frame-address span in Debug and
ASan (`regression_stack_*`). Completions delivered synchronously inside
`start()` because the condition already holds (event already set, mutex free,
shared task done) are invoked directly; the M1 driver loop absorbs them.

Corollary: a trampoline-delivered completion must not block waiting for work that
is queued behind it on the same thread.

## The waiter protocol (`detail/async_op.hpp`, `detail/wait_core.hpp`)

Waiters (event/shared-task waiters, mutex lock operations, `delay`) derive from
`detail::async_op`, a task holding a `detail::wait_core` (embedded `work_item`,
receiver, `cancel_registration`, result slot, one atomic `phase` byte). Since M3
the protocol lives in `wait_core` so that stream sources can host their
consumer's readiness wait without being tasks themselves; sizes are unchanged. A waiter is *published* (linked into the
primitive under its lock, or scheduled on a timer) before its cancellation
registration is armed, because arming may run the callback inline and the
callback must find the node. Publication, however, lets a grant race with
arming. The phase word decides who completes:

```
starter:  receiver_ = done; phase = started (| armed if the token is empty)
          link under lock; unlock
          cancellation_.arm(token);   old = phase.fetch_or(armed);    if (old & resolved) finish()
resolver: result_ = r;                old = phase.fetch_or(resolved); if (old & armed)    finish()
finish:   cancellation_.disarm();  handoff(deliver)
deliver:  take result, phase = 0, call receiver
```

Exactly one side observes the other's bit, so exactly one calls `finish`. With
an empty token `armed` is set before publication, the starter never touches the
object after unlocking, and the resolver always finishes. Acquire/release on the
phase RMWs makes the resolver's result write visible to whichever side delivers.

There is exactly one resolver per start:
- **List waiters**: the party that *unlinks the node under the primitive's lock*
  (a grant, a `set`, an inner completion, or the waiter's cancellation callback).
  The cancellation callback locks, checks `linked`, unlinks, unlocks and resolves
  with `errc::cancelled`; if the node is already unlinked a grant has won, and the
  callback does nothing. Removal is O(1) (doubly linked intrusive list with a
  pointer to the predecessor's link).
- **Timer waiters (`delay`)**: whoever first sets the `decided` phase bit (timer
  callback or cancellation callback).

`finish` disarms before delivery. A foreign disarm waits for a cancellation
callback running on another thread; that callback has already lost (node unlinked
or `decided` taken) and only returns, and it never needs a lock the finisher
holds, so this cannot deadlock. A self-disarm inside the callback returns at once
(M1 semantics), so a cancellation callback may complete — and the receiver
destroy — the waiter. Debug builds assert in `~async_op` that the waiter is idle
(`phase == 0`).

## `event`

State: `set_` (atomic, for the lock-free `is_set` and fast already-set path),
`result_`, FIFO wait list, spin lock.

- `wait().start`: if `set_` (acquire) → complete synchronously with a copy of the
  result. Otherwise lock, re-check, `prepare`, link, unlock, arm.
- `set(r)`: lock; if already set, ignore (Debug asserts); store the result,
  publish `set_`, take the whole list (`take_all` marks every node unlinked —
  O(n) under the lock, so cancellation callbacks see them as granted) and copy the
  result handle; unlock. Then, inside a `handoff_scope`, resolve each node in
  FIFO order. `set` never touches the event after detaching: a receiver may reset
  or destroy it.
- **wait vs set**: the re-check under the lock orders them; a waiter linked
  before `set` is in the detached chain, one starting after sees `set_`.
- **cancel vs set**: whoever unlinks under the lock wins; exactly one completion
  (`regression_cancel_vs_set_race`, 20,000 × scale, both outcomes observed).
- **start/arm vs set**: covered by the phase protocol
  (`regression_start_vs_set_race_with_token`).
- `reset` requires no registered waiters and no concurrent set/wait. Debug asserts
  on destruction with registered waiters.

## `shared_task`

State: `idle → running → done` (atomic byte), result, wait list, lazily created
internal `cancel_source`, spin lock.

- First waiter start (state idle): under the lock, create or reset the internal
  source, move to `running`, link itself; after unlocking start the inner task
  with the internal token, then arm its own cancellation. The inner task may
  complete synchronously inside its `start`: it resolves the (unarmed) waiter, and
  the starter's arm step then delivers.
- Later waiters while running link; after `done` they complete synchronously.
- Inner completion: lock, store result, `done`, take all waiters; unlock; resolve
  them in a `handoff_scope`. No access to the shared task afterwards.
- A waiter's own cancellation unlinks it only; the inner task keeps running even
  with no waiters left. `cancel()` copies the internal source under the lock when
  running and requests it outside the lock. It must not race `reset()` (a late
  request could otherwise hit the next run).

## `async_mutex`

State: `locked_`, FIFO wait list, spin lock (24 bytes). Uncontended `lock_op`
and `try_lock` take the spin lock once and complete synchronously.

- `unlock`: under the lock pop the oldest waiter; if there is one, `locked_`
  stays true (ownership transfers directly: the mutex is never free while waiters
  exist, so there is no barging and `try_lock` fails); otherwise clear `locked_`.
  The grant is resolved outside the lock through the trampoline.
- **cancel vs grant**: decided by who unlinks under the lock. If the grant wins,
  the waiter completes successfully and owns the lock; a cancellation that lost
  does nothing (`regression_cancel_vs_grant_race`, both outcomes observed, lock
  never leaked).
- `guarded` is a `machine`: `after(lock_op)` → `after(inner)` → `unlock(); fail(result)`.
  The machine passes its token to both the lock wait and the inner task. A
  cancelled lock wait skips the inner task (no-argument step) and never unlocks.

## `async_shared_mutex`

State: `writer_`, `readers_`, one FIFO wait list of readers and writers (the node
carries an `exclusive` flag), spin lock. Invariant: waiters exist only while the
mutex is held.

- Exclusive acquire succeeds immediately iff no writer and no readers.
- Shared acquire succeeds immediately iff no writer **and no queued waiter**
  (writer preference: once anything is queued, new readers queue behind it).
- `admit()` (under the lock) runs after every release and after a cancellation:
  if the head is a writer and the mutex is free, grant it; otherwise grant every
  consecutive reader at the head (stopping at the next writer). Granted nodes are
  moved to a private chain and resolved outside the lock in a `handoff_scope`.
- Cancelling a queued writer that was holding readers back re-runs `admit()`, so
  those readers are admitted while other readers still hold the lock.
- Cancel vs grant is decided by unlinking under the lock, as for `async_mutex`.

## `when_all` / `when_any` (`detail/join.hpp`)

One engine (`detail::join_base`) for both. Children start with a token from an
internal `cancel_source` linked to the start token by a `cancel_registration`.
A single 64-bit atomic packs:

- bits 0–39: outstanding **units** — one per child, one held by the starter until
  every child has been started, one held by the `when_any` winner while it
  requests cancellation, and one held temporarily by the parent-cancellation
  callback. Whoever releases the last unit completes.
- bits 40+: number of child completions (`when_any` only).

Hot path: a successful `when_all` child does one `fetch_sub`; every `when_any`
child does one `fetch_add(1<<40 − 1)`, which both releases its unit and reports
whether it was first (old completion count 0 ⇒ winner). The winner records its
index and result, requests cancellation of the others (N > 1), then releases the
winner unit. A failing `when_all` child claims "first failure" with one
`exchange`, stores the error and (cancel_on_error) requests cancellation
**before** releasing its own unit.

Why units are held across `request_cancel()`: completion may restart the
combinator, and restart resets the internal source. If a request were still
iterating the old registration list, it could invoke callbacks of the *next* run.
Holding a unit makes completion impossible until every request has returned.

Parent cancellation callback: `fetch_add(1)` on the units. If the old unit count
was 0, completion has already been decided and its finisher is blocked in
`link.disarm()` waiting for this callback: undo the increment and return.
Otherwise record `parent_cancelled`, request the internal source, release the
unit (possibly completing from inside the callback; the finisher's disarm is then
a self-disarm and returns immediately).

Results: `when_all` — first failure by completion order; else `errc::cancelled`
if the parent callback ran; else success. `when_any` — the winner's result.
Zero children: `when_all` completes successfully and synchronously (even with a
cancelled token); `when_any` asserts in Debug (Release: success, `winner() ==
npos`). With `run_all` and an empty start token the children get an empty token
(no refcount traffic). `when_any` needs a per-child completion context to know
the index: `when_any<N>` embeds `std::array<slot, N>`; `when_any_range` allocates
its slots once at construction. `when_all` uses the combinator itself as context.

*M5 amendment* (`docs/M5-performance.md`). A `when_all_range` of 64 or more
children counts completions in 16 counters allocated at construction (child *i*
uses counter *i* mod 16, each on its own cache line); a counter's last child
releases one unit, so the unit count holds one unit per counter instead of one per
child. Failures are still recorded, and cancellation requested, before the child's
share is released.

## Timers (`timer.hpp`, `testing/manual_timer_service.hpp`)

`timer_service::entry` is the intrusive node: `fire` plus implementation-owned
pairing-heap links (`child`, `sibling`, `prev`), deadline, sequence number and a
`scheduled` flag. `detail::timer_queue` is a pairing heap ordered by
(deadline, sequence) — FIFO for equal deadlines — with O(1) insert and O(log n)
amortized pop and arbitrary removal, all iterative. It also keeps a short list
of stack-resident *firing records* (entry, thread) for callbacks in progress.

Firing: under the lock pop the earliest due entry, clear `scheduled`, push a
firing record; unlock; call `fire` (which may destroy or reschedule the entry —
the service never touches it afterwards); lock; drop the record.

`cancel(e)`: under the lock — scheduled ⇒ remove, return true; firing on another
thread ⇒ wait (manual: unlock/yield/relock; steady: condition variable notified
after each callback when cancellers wait) and re-check; firing on this thread or
not pending ⇒ false. So `false` guarantees the callback has finished (or is the
caller), which lets users implement structured completion.

`manual_timer_service`: time moves only in `advance`/`advance_to`; due entries
fire on the calling thread in (deadline, sequence) order; `now()` steps to each
entry's deadline while it fires, so periodic reschedules inside the window fire
in the same call. `steady_timer_service`: one thread waiting on a condition
variable until the earliest deadline; `schedule` notifies only if the new entry
became the earliest. The thread copies the deadline before `wait_until` (the entry
may be cancelled and destroyed during the wait — found by ASan). Destruction
stops and joins the thread; Debug asserts that nothing is scheduled.

## `delay`

An `async_op` that is also a timer entry. Start: `prepare`, schedule at
`now() + d`, arm. The timer callback and the cancellation callback race for the
`decided` bit:

- timer wins → `resolve({})`; a concurrent cancellation callback lost and only
  returns, so `finish`'s disarm cannot wait on anything that waits on it;
- cancellation wins → `timers.cancel(entry)` (waits for a losing callback in
  progress elsewhere, which just returns) → `resolve(cancelled)`.

## `with_deadline`

Units: inner, timer, starter (+1 temporarily for the parent callback); a CAS on an
`outcome` byte (undecided → inner/timer/parent) picks exactly one outcome.

- Inner completes: if it wins, store its result and `timers.cancel(entry)`; if
  that returns true the fire will never run and the inner path releases the timer
  unit on its behalf, otherwise the (losing) fire callback releases it. Release
  the inner unit.
- Timer fires: if it wins, request the internal source (inner observes
  cancellation). Always release the timer unit.
- Parent cancellation: unit protocol as in `join_base`; if it wins, request the
  internal source and cancel the timer (releasing the timer unit if removed).
- Last unit: disarm the parent link and complete with the inner result,
  `timed_out` or `cancelled`. Hence never before the inner task has completed and
  the timer callback is finished or cancelled, even if the inner ignores
  cancellation for a while.

No path waits while holding anything another path needs: `timers.cancel` only
waits for a fire callback that has already lost the outcome CAS and only releases
its unit.

## Adapters

`fn_task<F>`/`make_task` forward start to `F(done, token)`. `finally_task` starts
inner with the start token, then runs `cleanup(ctx, result)` and completes with
the same result (callable overload binds a non-owning reference). `reschedule`
posts its embedded `work_item` to an executor and completes from there.

## Resettable cancel source (minimal M1 addition)

Since M3 this is internal: `reset()` is private and reached only through
`detail::cancel_source_access::reset`, so cancellation stays one-way for users.
`reset()` clears the `requested` flag of the existing state under
its mutex, asserting that no registration is armed. Combinators (`when_all`,
`when_any`, `with_deadline`, `shared_task`) call it at the start of a run whose
previous run requested cancellation. Without it, every `when_any` run (whose
winner always requests cancellation) would allocate a fresh source, violating
"zero allocation per operation". Precondition: every task that observed the
previous token has completed and no request is in progress — guaranteed by the
unit protocol above. Stale token copies (for example retained by idle machines)
observe the reset; they belong to completed work.

## `task::start` always inlined (codegen-only M1 change)

With the M2 headers in the same translation unit (for example through
`stepwright.hpp`), GCC 14 stopped inlining the two-line `task::start` forwarder into
the machine driver: `BM_after_sync_child` on Linux went from 5.10 to 5.97 ns/step
with an unchanged engine (measured against the committed M1 tree in the same
container). `task::start` now carries `STEPWRIGHT_ALWAYS_INLINE` (feature-tested,
plain `inline` fallback), as M1 already does for handle destructors. Behaviour
and layout are unchanged; the M1 numbers are restored (5.23 ns) and the M2 paths
gain as well.

## Stress and timing

All race tests use `test_support::race_rounds`, persistent threads released per
round through atomics only: on macOS, libc++'s `std::barrier` lives in an
uninstrumented library, and TSan reported the harness's pointer hand-offs as
races. Default iteration counts (20,000 × `STEPWRIGHT_STRESS_SCALE` for the
cancel-vs-grant/set/completion races; 2,000 × scale for real-time timer races)
keep the full Debug and TSan suites well under a minute.
