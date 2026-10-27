# Convenience composition: storage, completion and cancellation

The convenience layer uses the existing task, machine and waiter protocols. It
does not add opcodes, runtime member-function pointers or allocations on successful
steps, waits or runs. Preconditions and storage are independent of `NDEBUG`.

## Blocking bridge and teardown

`sync_wait` owns its receiver, mutex, condition variable, result and flag on the
calling stack. The receiver copies the borrowed error, sets the predicate and
notifies **under the mutex**. The unlock is the receiver's last access to the
waiter. The waiter must reacquire that mutex before it can return, so notification
and every receiver access happen-before destruction of the stack state. Spurious
wakes and synchronous completions use the same predicate. No callback or child
start occurs under this mutex: only the internal predicate and notification do.
An atomic store followed by notify would not provide this lifetime edge.

Debug checks reject pool workers and an active hand-off trampoline before starting
the task: blocking there can strand the work needed for completion. They cannot
detect arbitrary dependency cycles. Manual/virtual work must be driven, not blocked.

`virtual_executor::run` checks completion after each item/timer, before advancing
time again. It does not drain unrelated work after the target completes. A stalled
task remains active, so its receiver cannot live in a returned stack frame. The
executor instead retains one run's receiver/result state; cancellation followed
by `run` on the same task settles that start. A release completion flag publishes
the result and is the callback's final executor access. The caller-driven service
must still outlive every driving call and all its other users. Nested/concurrent
`run(task)` is disallowed, without changing nested `run_*` behavior.

## Address storage and reuse

Container constructors copy addresses into a vector sized once at construction;
pointer spans continue to be borrowed. Neither approach moves children. Join
slots/shards and cancellation barriers are unchanged, so completion still waits
for all children and cancellation requests. Elements must keep stable addresses
for the wrapper's lifetime. Temporary object ranges are rejected by overloads.
Sized ranges give a count directly; unsized forward ranges are counted in a
first pass. Unsized single-pass ranges are excluded to keep allocation bounded.

`with` on primitive operations checks the existing waiter phase before changing
values/durations. Replacement destroys and nothrow-move-constructs the stored
value, supporting nonassignable item types without allocation. Configuration is
serialized with start, never concurrent mutation. Deadline configuration checks
its outstanding-unit count. Completion has already disarmed callbacks and returned
the phase/count to idle, so reconfiguration from a receiver is valid too.

## Sequential composition and stack depth

`sequence` and `sequence_range` share a machine holding only a span and index.
Begin resets the index; a no-argument continuation starts the next child with the
parent token. Failure bypasses that continuation. The token is checked before
each start; empty sequences complete successfully even with a requested token.
The underlying driver's running-owner handshake absorbs synchronous completions
into its loop. There is no recursive start/completion chain, regardless of child
count or optimization. A started child completes before the next start or terminal
receiver, so no child outlives the sequence. Destruction/restart from the terminal
receiver inherits the machine's no-access-after-completion guarantee.

## Retry and cancellable backoff

`retry` is another machine, borrowing one restartable child and embedding `delay`.
Its error-taking continuation explicitly handles a failure; success or exhaustion
returns a terminal transition. Otherwise it configures the idle delay and awaits
it with the original start token. Cancellation during backoff removes/settles the
timer via the existing publish/arm/resolve protocol. The no-argument continuation
is skipped on that cancelled result, so the child is not started again. A request
between the delay and child is checked before starting the attempt. Cancellation
remains cooperative during inner work: an inner that ignores it must still finish.

Only the exclusive driver mutates attempts/backoff. Each run resets both. The
next delay is capped before converting a widened floating count to clock ticks,
avoiding integral overflow, and jitter is intentionally absent. Synchronous inner
completions and inline timer delivery still use the machine driver, keeping stack
depth constant. Nothing in the wrapper allocates; errors/context and user tasks
retain their existing allocation rules. No internal lock spans a predicate call,
child start or completion. The wrapper cannot complete with a live child or delay.
