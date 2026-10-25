# Performance design and memory trade-offs

The [benchmark method](../../benchmarks/README.md) defines each workload and fair
baseline; [published results](../benchmarks.md) are regenerated from JSON. This
document explains the mechanisms, not universal performance promises.

## Lazy cancellation registration

The default machine cancellation hook only wakes a current park. Registering it
at every start would contend on a shared source for children that only defer and
finish. Machines therefore bind the token at start and activate default
registration at their first park, before publishing `parked`. A custom
`on_cancel` hook still activates at start because it may cancel external work.

Park entry is: activate, SC-publish parked, SC-query requested. A request before
activation invokes the hook inline (which may see no park yet), and the final
query sees the request. A later request finds the registration; the
[engine ordering proof](engine.md#formal-parkrequest-ordering) then applies.
Re-parking reuses the registration. `wait_core` similarly binds/activates an idle
node rather than redundantly disarming it first. No waiter size changes.

## Why keep eight cancellation stripes?

One source owns one 1,280-byte allocation: reference count on one 128-byte unit,
request/wait bookkeeping on another, and eight 128-byte list stripes. Token copies
write a separate line from registration links. Each registration hashes its own
address multiplicatively to select a stripe, keeping its node at 40 bytes without
pointer tagging. Eight short spin locks avoid serialising independent workers.

The registration-heavy `BM_shared_token_park_workers` starts and wakes 128 parked
machines per pool worker on one shared token. All workers meet a start gate, then
register and deregister concurrently. Construction is outside the timed region;
dispatch and rendezvous overhead are included equally. A comparison build uses
one unpadded stripe and keeps the reference count separate: **256 bytes total**,
the same algorithm, ordering, pool, flags and workload otherwise.

Apple M4 Max, native AppleClang 16 and Linux arm64 GCC 14, Release, seven
repetitions with at least 0.5 s each, run sequentially with no build jobs active.
Median wall ns per machine (registration + wake/completion):

| Workers | macOS eight stripes | macOS compact | Linux eight stripes | Linux compact |
|---|---:|---:|---:|---:|
| 1 | 57.4 | 57.3 | 262.4 | 252.7 |
| 4 | 117.3 | 129.8 | 213.7 | 185.2 |
| 8 | 176.1 | 405.9 | 269.7 | 487.4 |

The compact state saves 1,024 bytes per source but is **2.31× slower on eight
workers on macOS, 1.81× on Linux** in this workload. At four workers the compact
layout wins on Linux, so striping is not a universal speed-up. Lazy registration removes irrelevant registrations,
not contention when many real waits share a token. We retain eight stripes for
the documented concurrent-wait use case. Sources are shared by a tree of tasks,
not allocated per machine or per wait; 10,000 sources would still consume about
12.2 MiB of state, so source count is a meaningful application design choice.
No lazy stripe allocation is introduced on the hot path. Dispatch dominates small
batches, and OS scheduling affects absolute costs. Raw comparison records and
reproduction details are in [the comparison data](../../benchmarks/results/2026-10-25/comparisons/README.md).

### Request versus activation

Each stripe has `occupied`, SC-written while linking and clearing the last node.
Activation links, SC-stores occupied, then SC-loads requested under that stripe's
lock. A requester first SC-exchanges requested, then SC-loads each occupied flag
and skips empty stripes. If it misses a registration, that load precedes the
activation's store in the SC order, so activation's later requested load must see
true and invoke inline. If it sees the stripe, the lock arbitrates unlinking.
Only the party that unlinks runs the callback, outside every lock. Stale true
only costs a lock. This is a flag/flag handshake, not reliance on eventual caches.

Foreign disarm increments waiters and snapshots finished under the stripe lock,
then unlocks and `atomic::wait`s. Invocation clears its record under the same
lock, increments finished if anyone waits, unlocks and notifies. Either the waiter
registers first and sees a changed value, or finds no invocation and never waits.
Another stripe's notification is harmless: the waiter rechecks its node. A local
requester token protects state through notification. Self-disarm clears the stack
record before self-destruction or rearm; no returning invocation touches a freed
node. All arm/disarm/request paths allocate nothing.

## Sharded structured completion

`when_all_range` with at least 64 children allocates 16 cache-separated completion
counters once. Child i decrements counter i mod 16; only the counter's last child
releases a unit of the join's shared count. A successful child need not even read
the owner pointer until it is last. Failure recording and cancellation happen
before releasing the child's share. Acquire/release decrements chain all child
writes through the shard's last release to final completion. Fixed small joins
and `when_any` keep their single packed counter. Runs still allocate nothing.

## Pool hot paths and liveness

The [runtime argument](runtime.md) describes the bounded rings, separate polled
and owner-private cache lines, conservative `head_seen`, quarter-worker search cap,
batch transfer announcements and search hand-over. Idle workers use interruptible,
timed guard waits while work runs. The mode decision shares the sleeping CAS,
rather than imposing stronger operations per local post. Even a blocked poster's
single item can run on an idle worker; completely idle pools do not poll.

### Measured liveness cost

Against the previous committed pool (`d9be96b`), built separately with the same
compiler/flags on the same machine, seven repetitions of at least 0.5 s each:

| Workload | macOS before | macOS hardened | Linux before | Linux hardened |
|---|---:|---:|---:|---:|
| Worker post / 1 | 3.97 | 3.85 | 4.02 | 4.08 |
| Worker post / 2 | 2.01 | 1.99 | 2.04 | 2.07 |
| Worker post / 4 | 1.03 | 1.03 | 1.06 | 1.07 |
| Worker post / 8 | 0.57 | 0.58 | 0.60 | 0.61 |
| Ping-pong | 38.58 | 38.90 | 32.45 | 32.81 |
| Trivial fan-out / 8 | 25.66 | 23.04 | 24.35 | 25.93 |
| Trivial fan-out / 1 | 18.02 | 17.74 | 19.60 | 19.45 |
| Working fan-out / 8 | 167.37 | 153.56 | 86.17 | 87.11 |
| Working fan-out / 1 | 1413.07 | 1090.03 | 526.91 | 547.60 |

All values are wall ns/item. The largest regression is 2.5% on macOS and 6.5%
on Linux, within the 10% budget. Apparent improvements, especially for working
leaves, include scheduler/core-frequency variation; they are not guaranteed wins.

A trivial fan-out is bottlenecked by the worker starting and posting every child;
more workers add cache traffic, not useful computation. Real leaf work amortises
that overhead. Coroutines with visible, elided child frames can be faster than
explicit transitions; opaque heap-allocated frames measure a different trade-off.
The benchmark suite reports both, including observed frame allocations. Do not
infer asynchronous or cancellation performance from synchronous-only baselines.
