# Streams: pull, readiness and backpressure

The public protocol is `source<T>` plus `pull<T>` in `stream/source.hpp`.
The [guide](../guide.md#streams) describes usage. An item must be nothrow movable
and destructible; broadcast additionally requires nothrow copying.

## Consumer protocol

One consumer calls `try_pull`: item, pending, end or failure. End and failure are
sticky. On pending it registers exactly one `on_ready` notification (or starts a
`ready_op` task). It must not pull or register again until that notification
completes. A cancelled readiness wait unregisters, but does not inherently cancel
the source. Sources have stable identity and cannot be copied or moved.

Readiness already satisfied completes synchronously; readiness caused by a state
change uses the [hand-off trampoline](primitives.md). Waiter storage is embedded;
nothing allocates per readiness round. Channel and broadcast item constructors
and destructors run under their locks and must not throw or re-enter them.
No completion or other callback runs under those locks.

## `source_machine`

The source driver uses the [engine protocol](engine.md), with independent wake,
child and deferred readiness and distinct wake/child failure slots. Extra bits:
`busy` (a pull's run), `item_ready` (an asynchronously produced item), `terminal`
(sticky outcome), `waiting` (consumer readiness registered).

`owned_` and `held_` are consumer-only flags. Initially the consumer owns the driver
and `next_` names `begin`. A synchronous `emit` leaves an inline item and next-step
thunk; pulling moves it out without publishing. A source that never suspends does
no atomic RMW per item. If a child/park/defer suspends, the driver releases
`running`, leaves `busy` and clears `owned_` on the consumer thread.

An asynchronous emit publishes `item_ready` and clears `busy|running|waiting` in
one CAS. If it removed `waiting`, it resolves the consumer's wait and makes no
further access: that consumer may destroy the source. Taking the item CASes back
to `busy|running`, returning driver ownership to the consumer. A readiness check
between pulls may run ahead to one item and mark `held_` until the next pull.

Readiness registration prepares `wait_core`, then CASes in `waiting` only if no
item or terminal outcome is published. Registration and publication cannot miss
each other on one atomic word. Cancellation clears `waiting` by CAS; whoever
removes the bit resolves the wait. A cancelled wait does not affect a running
child or buffered item. `set_token` separately controls children and parked runs;
the readiness token never replaces it. Park/request ordering is the engine's SC
handshake. Deferral off the home executor releases ownership before posting.

Finish adds context and publishes a sticky terminal error (empty means end),
waiting for any claimed-but-unpublished wake first. Later wakes are discarded.
Destruction is valid before the first pull, between nonsuspended pulls, or after
end/failure, never during a readiness wait or suspended child/park/defer. A
buffered item is destroyed with the source. A consumer that stops early must
separately settle any source operation still suspended.

## Channels

One spin lock guards the ring, closed/reason state, consumer-wait flag and FIFO
senders. A bounded channel allocates its ring once. An unbounded ring doubles on
growth (minimum eight); moved-from retired storage is freed after unlocking.

Senders queue only while a bounded ring is full. A pull removes the oldest item
and admits the oldest waiting sender into the freed slot in the same critical
section; no try-send can overtake it. Unlinking decides admission versus
cancellation. A failed/cancelled send retains its value in the send operation.

A consumer's empty check and wait publication happen in the same critical
section. Send or close exchanges its wait flag to false under that lock; exactly
one producer resolves it after unlocking. Close is idempotent (first reason
wins), detaches waiting senders with `closed`, and wakes the consumer. Buffered
items drain before end/failure. Cancelled/destroyed consumer waiters leave no
reference in the channel.

## Merge

`merge` rotates its polling index after each item, retires ended inputs and keeps
the first polled failure sticky. If all live inputs are pending, one readiness
round arms **all** live inputs, so a slow first input cannot block a ready later
one. Slots are inline for fixed merges and allocated once for range merges.

The round counts one unit per live input plus the starter and temporarily the
consumer-cancellation callback. First readiness chooses `ready` by CAS and
requests the internal source to cancel other input waits, holding its unit across
the request. Consumer cancellation similarly chooses `cancelled`. Last-unit
release resolves the consumer's `wait_core`; thus every input wait and request
has finished before a new round may reset the internal source. A synchronous
readiness result can stop arming early; unarmed units are explicitly released.
This cancels input *waits*, not any independently running input producer.

## Broadcast

A fixed ring stores `{remaining readers, value}` with head/tail sequence numbers.
Subscriptions start at tail and each carry a cursor, dropped count and embedded
waiter. All accounting is under one lock. Publish assigns the current subscriber
count and detaches waiting subscribers; with none, the item is discarded.

Pull copies an item, advances that cursor and decrements its remaining count.
Fully read head slots are released; each slot is released once, amortised O(1)
per item, without scanning subscribers. In `overflow::wait`, freeing space admits
the sole waiting publisher under the same lock. In `drop_oldest`, a full publish
evicts the oldest slot. A lagging subscriber catches up to head on its next
operation and counts the skipped sequence gap. It never decrements an evicted
slot's fresh count. Unsubscribe decrements unread retained slots (bounded by
capacity), releases space and admits a waiting publisher. Close preserves retained
items for draining, wakes readers and resolves a pending publisher with `closed`.

## Factories, consuming and tests

`empty`, inline `just`, borrowed-lvalue `from_range` and `generate` are always
ready. A generator ends permanently at `nullopt`. `interval` uses the embedded
timer/wait protocol; tick k is due at start + (k+1)×period. Late pulls coalesce
missed ticks, emitting the most recent tick number rather than an unbounded
backlog. Readiness cancellation removes its timer.

`for_each` loops over available items in one machine step, checking cancellation
before every pull. Pending returns `after(ready_op)`. A callback may return void,
false to stop successfully, or an error. Synchronous item chains and asynchronous
readiness chains both have constant stack depth.

Tests cover send-versus-wait lost wakes, concurrent producers, cancelled consumers,
merge head-of-line blocking, broadcast loss/backpressure, source cancellation
separation, and a multi-threaded chaos pipeline combining locks, deadlines,
channels and merging. `STEPWRIGHT_STRESS_SCALE` scales race rounds.
