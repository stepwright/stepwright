# Engine: ownership, cancellation and lifetime

The implementation is in `include/stepwright/machine.hpp` and `cancel.hpp`.
For the public contracts and complete programs, start with the [guide](../guide.md).

## Exclusive driver ownership

One atomic word contains `active`, `running`, `claimed`, `wake_ready`, `parked`,
`deferring`, `deferred_ready`, `child_ready` and `child_failed`. Only the owner of
`running` executes steps. Acquire/release read-modify-write operations transfer
ownership and make the previous step's writes visible. Layout does not depend on
`NDEBUG`; the step guard's storage exists even when checks are disabled.

Steps return transitions to one loop. Synchronous child completions publish a
result but never recursively enter a second driver. Deferral through an immediate
executor also signals that loop. Million-step chains therefore use constant stack
depth in Debug as well as optimised builds, without tail-call optimisation.

## Independent result channels

A wake first CASes in `claimed`. The winner writes the wake error, then publishes
`wake_ready`; duplicates discard their errors. Publication acquires `running`
only when the machine is parked and the old driver has released ownership.
Otherwise the wake remains buffered, including across child waits and deferrals.
The driver's release CAS and the publication CAS cannot both miss each other.

`wake_if_parked` requires `parked && !claimed` in the **same CAS** that claims the
slot. An earlier check is only an optimisation, never acceptance. Cancellation
therefore cannot impersonate a child completion if a real wake advanced the
machine while the cancellation error was being constructed. Ordinary `wake`
intentionally targets the current *or next* park.

The driver moves the wake error before clearing `claimed|parked|wake_ready`.
Otherwise a new wake could overwrite the handle while it was being read. Once a
wake is consumed, a new one belongs to a later park; unused wakes are dropped on
completion. These APIs do not provide lifetime ownership: stop external producers
before destroying the object, even when using conditional delivery.

Children complete exactly once by the task contract. Failure alone writes the
separate child-result slot; a successful child writes no error handle. One
`fetch_or(child_ready|running|optional child_failed)` publishes the result. After
`start`, the driver either consumes ready/failure bits in one RMW, or CASes to
consume a newly arrived result or drop `running`. Only a result from that child
can advance its continuation. Neither consuming a child result nor deferring
clears a pending wake. The next child cannot start before the old child error has
been moved, so its writer cannot overlap that read.

## Executor affinity and deferral

Asynchronous dispatch posts the embedded work item when the home executor says
the delivering thread is foreign. Otherwise it resumes inline. This is exclusivity,
not affinity to a particular worker of a multi-threaded executor.

On an executor thread, `defer` posts while still owning `running`: an inline
executor signals `deferred_ready`, which the loop consumes without recursion.
Off the executor, it must drop `running` **before** posting and touch nothing
afterwards. Otherwise a fast worker could signal the old driver and the deferred
step would incorrectly continue on the foreign posting thread. Executors that may
invoke inside `post` must report `running_in_this_thread()` for that invocation.
Wake readiness is independent of deferred readiness.

## Cancellation registration

The state has eight address-hashed, independently spin-locked intrusive lists.
Each node keeps a pointer to its predecessor's link: unlinking head, middle or
tail is O(1). When linked, a union holds the next node; when unlinked, it holds
a pointer to a stack invocation record (or null). `previous_` selects the mode.
See [performance](performance.md) for layout measurements and the request/arm
handshake.

Under the stripe lock, a requester unlinks a node, snapshots function/context and
publishes its invocation record, then unlocks before calling. Foreign disarm
waits for that record to detach. Self-disarm clears both the record's node and the
node's record pointer; a callback may destroy or rearm itself. On return the
requester touches the node only if the record still names it. Its local token
keeps state alive through invocation, unlocking and notification. Empty tokens
perform no reference-count or lock operations.

A machine with a custom `on_cancel()` registers at start; an already-requested
token invokes it before `begin`. The default hook only acts on parks, so its
registration activates at the first park, before publication, and remains active
until completion. Children always receive the bound token, including before that
first registration. A request before activation runs the hook inline; park entry
then rechecks the request flag. Custom hooks run on the requester and must be
thread-safe, including against steps and cancellation before the first step.

## Formal park/request ordering

For nonempty tokens, these operations are sequentially consistent:

1. Driver publishes `parked` with `fetch_or` on the machine state.
2. Driver loads the token's requested flag.
3. Requester exchanges that flag to true, before callbacks.
4. Default callback loads the machine state before conditional delivery.

They share one SC total order. If (2) sees false, it precedes (3), which precedes
(4); (4) therefore follows (1) and cannot see an older unparked state. If (4)
precedes (1), (2) must see the request. A later operation clearing `parked` means
a genuine wake already won that park. Thus neither cancellation can be lost nor
a cancellation wake accepted for a different wait. Empty-token parks keep the
acquire/release path, without SC requested-flag operations or fences.

## Completion and tests

Finish disarms cancellation (waiting for foreign callbacks), waits for any wake
that claimed but has not published, closes active state and discards unused wake
data. It adds failure context, invokes `annotate` and `on_complete` when present,
then calls the receiver. There is no access after the receiver, even in a resume
thunk: the receiver may destroy or restart the machine. Exceptions escaping steps
or hooks terminate; they are not an error channel. No user callback, child start
or executor post occurs under an internal lock.

Tests cover destruction from completion, lazy registration, already-requested
tokens, cancellation/park ordering, early replies across child waits and deferrals,
and four-producer chains carrying a non-atomic payload across 100,000 children.
Race counts scale with `STEPWRIGHT_STRESS_SCALE`; the default suite stays bounded.
