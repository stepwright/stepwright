# M3 streams: protocol, state machines and races

Independent design from the M3 specification (`.claude/plans/spec-M3-streams.md`)
on top of M1 (`docs/M1-concurrency.md`) and M2 (`docs/M2-primitives.md`). No M1
or M2 guarantee is weakened; M1/M2 layouts are unchanged.

Common rules (as in M2):

- Waiter state is embedded: every readiness wait, waiting sender and waiting
  publisher lives in the waiting object or the source. Nothing is allocated per
  item, per wait or per readiness round in steady state. Allocations: a bounded
  channel's ring (once), an unbounded channel's ring (on growth only), a
  broadcast's ring (once), the merge's internal cancel source (once) and
  `merge_range`'s per-input slots (once).
- Internal locks are the M2 spin lock around buffer/list/flag updates. No
  completion is delivered and no user callback or step runs under a lock. The
  item type's move/copy constructor and destructor do run under the channel or
  broadcast lock (documented; they must not throw or re-enter).
- Completions caused by a state change (send, publish, close, timer, input
  readiness) are delivered through the M2 hand-off trampoline, so chains of
  readiness use constant stack. Readiness that already holds is delivered
  synchronously inside `on_ready`.
- Nothing completes while anything it started is outstanding.

## Carry-over from M2

**One-way cancellation.** `cancel_source::reset()` is now private. The
combinators that own and reuse a source (`when_all`/`when_any`, `with_deadline`,
`shared_task`, and M3's `merge`) reach it through
`detail::cancel_source_access::reset`, as do the tests. Behaviour is unchanged;
`cancel.regression_cancellation_is_one_way_publicly` checks with a concept that
`source.reset()` is not well-formed for users.

**Shared waiter core.** The M2 completion protocol (publish, then arm; the
`armed`/`resolved` phase bits decide which side delivers; delivery through the
trampoline) moved from `detail::async_op` into `detail::wait_core`, a non-task
object. `async_op` is now a task holding a `wait_core`; every M2 type keeps its
size (checked by the layout test). Stream readiness waits use `wait_core`
directly, because a source is not itself the task being started: one source
hosts the readiness wait of its consumer. `wait_core::unprepare()` abandons a
prepared but never published wait (used when a registration CAS finds the
source ready). `detail::cancelled_error()` is a shared `errc::cancelled` value so
that cancelled readiness waits (routine in merge rounds) do not allocate;
`error` is copy-on-write, so adding context to a copy never touches it.

**Chaos test.** `tests/test_chaos.cpp` (see *Stress*).

## Protocol (`stream/source.hpp`)

`pull<T>` is a tagged union of `T` and `error` plus a one-byte state (`item`,
`pending`, `end`, `failed`); 16 bytes for `int`. Factories: `pull::of(v)`,
`pull::pending()`, `pull::end()`, `pull::failure(e)` (an empty `e` gives `end`,
which lets sources store one sticky "terminal" error for both outcomes).

`source<T>` has two virtual functions, `try_pull()` and `on_ready(done, token)`,
and `ready()` returns a `ready_op` task (16 bytes) whose `start` forwards to
`on_ready`. A machine waits with `after<&D::f>(ready_op_member)`. Rules, beyond
spec §2:

- The single consumer calls neither `try_pull` nor `on_ready` while its readiness
  wait is outstanding (it may cancel it through the token). All sources assert
  this in Debug through `wait_core::idle()`. The phase is reset with release
  ordering before the receiver runs, and the receiver's own synchronisation with
  the consumer makes that visible.
- Sources have identity (not copyable or movable). Factories return prvalues.

## `source_machine<D, T>`

A separate driver (not `machine<D>`'s), with the same building blocks: one atomic
state word, independent readiness bits for wake/child/deferral, separate wake and
child-failure slots, the same SC park/cancel handshake. `machine<D>` is untouched.

### State

State word bits: `running`, `claimed`, `wake_ready`, `parked`, `deferring`,
`deferred_ready`, `child_ready`, `child_failed` (as in M1) plus

- `busy`: a run is in progress: set from the pull that starts a run until its
  outcome is published;
- `item_ready`: an item produced by an *asynchronous* run is in the buffer;
- `terminal`: `terminal_` holds the sticky outcome (empty error = end);
- `waiting`: the consumer's readiness wait is registered.

Two consumer-only plain fields avoid atomics on the synchronous path:

- `owned_`: the consumer owns the driver between pulls (state has
  `busy|running`). Initially true (nothing has run; the next step is `begin`).
- `held_`: the buffer holds an item produced by a synchronous run that
  `on_ready` reported but nobody has pulled yet.

### Runs

`drive<Sync>(step)` is the M1 driver loop with two extra transitions:

- `emit<&D::f>(v)` constructs `v` in the one-item buffer and returns a transition
  whose thunk becomes `next_`, the step run by the following pull.
- `end()`/`fail(e)` finish the stream: add context to failures, store
  `terminal_`, wait for an in-flight claimed-but-unpublished wake (as
  `machine::finish` does), drop a buffered wake, and CAS the state to exactly
  `terminal`. Later wakes see `terminal` and are dropped.

**Synchronous run** (consumer thread; `try_pull` or `on_ready` while `owned_`):
the consumer already owns `busy|running`, so an emit simply leaves the item in
the buffer and returns; `try_pull` moves it out. **A source that never suspends
performs no atomic read-modify-write per item.** If the run suspends (child not
complete, park without a wake, deferral), the release of `running` is the usual
M1 CAS, `busy` stays set, and the consumer clears `owned_`.

**Asynchronous run** (the thread that completed the child or wake, or the home
executor): continues the loop; an emit or finish is *published* with one CAS that
clears `busy|running|waiting` and sets `item_ready` (or `terminal`). If the CAS
removed `waiting`, the publisher resolves the consumer's readiness wait. After the
CAS the driver touches nothing (the consumer may take the item and destroy the
source).

**Taking a published item**: one CAS clears `item_ready` and sets
`busy|running`, so the consumer owns the driver again for the next pull.

### Races

- *Consumer registers vs async publication.* `on_ready` (when not owned) prepares
  the wait and CASes `waiting` in only if neither `item_ready` nor `terminal` is
  set; otherwise it unprepares and completes synchronously. Publication clears
  `waiting` in the same CAS that publishes. The two CASes on one word are totally
  ordered: either the publisher sees `waiting` and resolves, or the consumer sees
  the outcome. A boundary state (`!busy`, nothing published) cannot be observed
  there because only the consumer leaves `busy` cleared without an outcome.
- *Cancel of the readiness wait vs publication.* The cancellation callback
  clears `waiting` by CAS; whoever removes the bit resolves (the `wait_core`
  phase protocol then handles a resolution that precedes arming). The source's
  run, its children and its buffered item are unaffected.
- *Wake vs park, child completion vs suspension, deferral*: the M1 protocol
  verbatim (claim, publish with ownership hand-off only for a suspended park;
  child readiness never claims; `deferred_ready` separate). A wake that arrives
  while the consumer owns the driver between pulls is buffered for the next park.
- *Cancellation through `set_token`*: children get this token; the default
  callback conditionally wakes a current park with `errc::cancelled`, and a park
  re-checks the token after the SC park publication (M1's formal argument).

### Lifetime

Destroy when not suspended: before the first pull, between pulls of a source not
waiting on anything, or after end/failure, and never with a registered readiness
wait. Debug asserts `(!busy || owned_)` and an idle readiness wait. A buffered
item is destroyed with the source.

## `channel<T>`

State under one spin lock: ring buffer (`head`, `count`, raw storage), `closed`,
`reason`, `waiting` (consumer wait registered), FIFO list of waiting `send_op`s.
The consumer's `wait_core` lives in the channel.

- **Invariant**: senders are queued only while a bounded buffer is full. When the
  consumer pops an item it moves the oldest queued sender's value into the freed
  slot in the same critical section, and resolves that sender after unlocking.
  Hence FIFO admission and no overtaking by `try_send` (which fails when full).
- **Producer vs consumer wait**: `on_ready` checks "item or closed" and sets
  `waiting` in one critical section. A producer that enqueues does
  `wake = exchange(waiting, false)` in its critical section and resolves after
  unlocking. Exactly one producer can observe `waiting == true` per registration
  (`regression_concurrent_senders_wake_once`). No lost wake-up: the check and the
  registration are atomic with respect to every enqueue
  (`regression_send_vs_consumer_wait`).
- **Close vs send**: `close` sets `closed` under the lock, detaches all senders
  (they complete with `errc::closed`, values remain in their `send_op`s) and takes
  the consumer wait. A send that locked first enqueued normally; one that locks
  later fails. The consumer drains buffered items, then gets `end`/`failed`.
- **Cancellation**: a waiting sender's callback unlinks it under the lock if still
  linked (otherwise it was granted or closed: it does nothing). The consumer
  wait's callback clears `waiting` if still set. Whoever unlinks/clears resolves.
- **Unbounded growth**: allocate (doubling, minimum 8) under the lock when full,
  move the items, free the old block after unlocking.
- A consumer that cancelled its wait and was destroyed leaves no reference in
  the channel; producers continue (`regression_consumer_destroyed_after_cancel`).

## `merge<T, N>` / `merge_range<T>`

Per input slot: `{owner, input, live}`. Consumer-only state: `live_`, `next_`
(fairness index), sticky `failed_`/`failure_`. Round state: `units_`,
`decision_` (`undecided`/`ready`/`cancelled`), internal `cancel_source`, the
consumer's `wait_core`.

- **try_pull** polls live inputs from `next_`, returns the first item and sets
  `next_` past that input (rotation), retires inputs that end, records the first
  failure (sticky, polling stops) and returns `end` once no input is live.
- **Readiness round** (`on_ready` when nothing is ready): reset the internal
  source if the previous round requested it; `units = live + 1`; prepare the
  consumer wait; arm `on_ready` on every live input (from `next_`) with the
  internal token, each completing into `on_input(slot)`. Arming stops early once
  the round is decided (an input was ready synchronously). Release the starter's
  unit plus the unarmed inputs' units; then arm the consumer's cancellation.
- **on_input**: unless the input wait was cancelled, try `decision: undecided →
  ready`; the winner requests the internal source, which cancels every other
  input wait (each completes, through its own protocol, with `errc::cancelled`).
  Release one unit. The unit is held across the request.
- **Consumer cancellation**: `fetch_add` a unit; if the count was zero the round
  has already completed and its finisher is in `disarm()` waiting for this
  callback: undo and return. Otherwise try `undecided → cancelled`, request the
  internal source, release the unit.
- **Completion**: whoever releases the last unit resolves the consumer wait
  (`errc::cancelled` iff the decision is `cancelled`). So the merge's readiness
  completes only after every input wait completed and every cancellation request
  returned, which also satisfies `reset()`'s precondition for the next round.
- **No head-of-line blocking**: every pending input is armed, so the first ready
  one completes the round (`regression_no_head_of_line_blocking`: 10 s and 1 s
  intervals on manual timers produce items at 1 s and 2 s).
- **Failure**: an input that fails becomes ready, wins the round and cancels the
  other waits; the next `try_pull` finds the failure. Outstanding input waits
  exist only inside a round, so "cancel the other waits on failure" is exactly
  the round's cancellation.

## `broadcast<T>`

Ring of `capacity` slots, each `{remaining, T}`; sequences `head` (oldest
retained) and `tail` (next to publish); subscriber count; waiting subscribers
(FIFO list); at most one waiting `publish_op` (single producer); all under one
spin lock. A subscription has a cursor (sequence), a `dropped` counter and its
`wait_core`.

- **Publish**: `remaining = subscribers` for the new slot; with no subscriber the
  item is discarded (`head = tail`). All waiting subscribers are detached and
  woken after unlocking (in a `handoff_scope`, FIFO).
- **Read**: copy the slot at the cursor under the lock, advance the cursor,
  decrement `remaining`; if it reaches 0, release fully read slots from the head
  (`while head != tail && slot(head).remaining == 0`). Each slot is released
  once, so eviction is amortised O(1) per item, independent of the number of
  subscribers; no scan over subscribers.
- **overflow::wait**: a full ring makes `publish_op` wait in `publisher_`; any
  release (read or unsubscribe) admits it in the same critical section (store
  its value, detach the waiting readers) and resolves it afterwards.
- **overflow::drop_oldest**: a full ring evicts the head slot regardless of
  `remaining`. A subscriber whose cursor is behind `head` adds the gap to
  `dropped()` and jumps to `head` (on its next pull or readiness check). Readers
  never decrement evicted slots; overwritten slots get a fresh count.
- **Unsubscribe** (destruction): decrement `remaining` for every retained slot
  this subscriber has not read (at most `capacity`), release, admit a waiting
  publisher. **Subscribe**: cursor = `tail`, count + 1. Both thread-safe.
- **Close**: wake every waiting subscriber, complete a waiting publish with
  `errc::closed`; subscribers drain retained items, then `end`/`failed`.
- Cancellation of a subscriber wait or a publish: unlink/clear under the lock if
  still registered, resolve if so (the M2 rule).

## Factories (`stream/sources.hpp`)

`empty`, `just` (values in an inline `std::array`), `from_range` (begin/end
iterators of a borrowed lvalue range; rvalues are rejected), `generate` (calls
`f()` per pull; `nullopt` ends, sticky). These are always ready: `on_ready`
completes synchronously. `interval(timers, period)` emits the number of the
latest due tick; tick k is due at `start + (k + 1) * period`. **Missed ticks
coalesce**: a late pull yields one item numbered with the most recent due tick, so
numbers skip by the missed periods. Readiness schedules the embedded timer entry
for the next tick; timer fire and cancellation race for the `decided` bit as in
`delay`.

## `for_each`

A `machine`: the entry step loops `try_pull` → `f(std::move(item))` without
leaving the loop while items are available, checks the token before each pull,
and on `pending` returns `after<&begin>(ready_op)`. The readiness wait gets the
machine's token, so cancellation cancels the wait first and the no-argument
continuation turns it into failure. Constant stack for synchronous chains (a
loop) and for asynchronous chains (trampoline + M1 driver).

## Stress

- `test_chaos`: four threads; each round runs `when_all` of four
  `with_deadline(async_mutex::guarded(critical))` on a `steady_timer_service`
  with random timeouts and random external cancellation; `critical` checks mutual
  exclusion, sends a tagged value to a shared bounded channel (cancellable while
  waiting for space) and then holds the lock for a random delay. A consumer
  machine drains `merge(channel, interval)` with `for_each`-style pulls on
  whatever thread delivers readiness. Checks: exclusion, exactly-once completion,
  no hang (10 s per round), results in {ok, timed_out, cancelled}, and that the
  consumer received exactly the successfully sent values (count and sum).
- Race tests use `test_support::race_rounds` (atomics-only round release).
  Default counts (20,000 × `STEPWRIGHT_STRESS_SCALE` for the lost-wake-up test)
  keep the Debug and TSan suites under a minute.
