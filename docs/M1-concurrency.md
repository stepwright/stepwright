# M1 concurrency and lifetime protocol

Independent design from the M1 specification; no implementation prior art
was consulted.

## Machine ownership

One atomic word contains `active`, `running`, `claimed`, `wake_ready`, `parked`,
`deferring`, `deferred_ready`, `child_ready`, and `child_failed` (no Debug-only
members). Wake, child and deferred readiness are independent bits, not one shared
signal: consuming one cannot satisfy or erase another wait. Start publishes
`active|running` before arming
cancellation. Only the owner of `running` executes steps. All hand-offs use
acquire/release RMWs so the previous owner's step writes and delivered data
are visible to the next owner.

### External wakes and conditional cancellation

A wake CASes in `claimed`; duplicates drop their errors without touching the
wake-result slot. The winner stores its error, then CAS-publishes `wake_ready`.
Only if `parked` is set and `running` is clear does that same publication
acquire `running`; the deliverer resumes or posts the embedded work item.
Otherwise the result stays buffered for the current/next park, even across
multiple children and deferrals. In particular, a suspended child wait or a
queued child continuation belongs to the child's actual completion, not the wake.
The driver's release CAS dropping
`running` and the deliverer's publication CAS cannot miss each other.

`wake_if_parked` checks `parked && !claimed` **in the same CAS that claims the
slot**. It returns false on idle, deferred, child and already-claimed waits.
Default cancellation uses this conditional call, including the cancellation
check when entering park. A preliminary parked check avoids needless error
allocation, but is never the acceptance decision. If a legitimate wake advances
to a child while cancellation constructs its error, cancellation's claim fails:
it cannot impersonate the child. Custom cancellation hooks should use the same
conditional API to cancel only a current park, or cancel external work that will
eventually wake the machine. Ordinary wake intentionally queues for a future park.

The driver moves the wake error **before** clearing claimed/parked/wake_ready;
otherwise an upcoming wake could overwrite a handle still being read. Ordinary
`wake` is legal at any time while active, including a child wait; the old
child-wait/pending-wake assertions are withdrawn. On entering park a published
wake is consumed inside the driver loop, without suspending. Producers must not deliver
late wakes after their object's lifetime ends; conditional delivery does not
provide lifetime ownership.

### Exactly-once child results

Child completion does not claim: the task contract guarantees exactly one call.
Failure handles alone are copied to a separate child-result slot. One
`fetch_or(child_ready|running|optional child_failed)` publishes the result and
hands ownership to the deliverer if the old driver has relinquished it.
Successful completions do not write an error handle at all.

After child.start returns, the driver's `fetch_and` both tests and consumes
ready/failure bits in one RMW. If not ready, a CAS loop atomically either consumes
a newly published result or drops `running` and returns. An asynchronous owner
consumes ready bits before continuing. There is no duplicate-claim RMW for a child,
nor a separate ready check followed by a consumption RMW on the synchronous path.
All transitions converge on one local result and one step-dispatch site, avoiding
duplicated temporary/error cleanup in the driver. Empty-handle destructors expose
their null check at the call site with a feature-tested inline hint (ordinary
inline fallback); nonempty release routines can still be outlined. This keeps
GCC's expanded driver from turning empty cleanup into per-step cold calls.

The child and wake error slots must be distinct: a reply may precede the send
completion, or arrive while the driver is still moving a child failure. Child
consumption clears only child-ready/failure bits; it neither clears the wake claim
nor reads the wake slot. A new child cannot start before that move and the following step finish,
so no second child writer can overlap that read. This also tests result visibility
in the 100,000-step four-producer chains with a non-atomic payload.

A transition contains two compile-time thunks: an ordinary step thunk for
the local loop and a resume thunk for asynchronous entry. Only the resume
thunk persists, in `work_item::run`; no separate saved step pointer is
necessary. The registration owns the token, avoiding another token member.
Together with compact cancellation linkage, these choices give a 112-byte
machine including both result slots, with no heap-backed engine state.

`claimed` stays set until consumption. Wakes arriving after consumption
belong to the *next* park; callers must stop using a machine before
destroying it. Waking an idle machine is a debug precondition failure.
An unused pending wake is discarded on completion, even if the operation never parks.

Completion leaves the step guard clear, disarms cancellation (waiting for foreign
callbacks), closes the active state and discards pending wakes,
invokes the optional hook, then invokes the receiver. No access to the
machine follows the receiver, including from resume thunks. Synchronous
children only publish; they never enter another driver. Exceptions
escaping user steps terminate.
If a wake has claimed but not published, finish yields until publication before
closing the slot; it must not race the write. Debug destruction asserts idle,
catching destruction with pending children, park producers or queued work.

Deferral uses separate `deferring` / `deferred_ready` bits, not the result
slot. An early wake stays buffered for the next park even across a deferred
step, and cannot resume the machine before its queued work item executes.
An immediately executing executor signals the existing driver instead of
recursively entering it.

*M4 amendment.* Posting while still owning `running` is right only when the
deferring thread belongs to the home executor (`running_in_this_thread()`): an
item run inline, or by another worker of the same executor, signals the driver,
which continues on an executor thread. From any other thread a multi-threaded
executor (the M4 `thread_pool`) could run the item before the driver suspended;
the item then only signalled readiness and the step continued on the *deferring*
thread, against "continue on the home executor". Off the executor the driver now
releases `running` (with `deferring` set) *before* posting, so whichever executor
thread runs the item takes ownership and runs the step; after the post nothing
touches the machine. Wakes still buffer (`parked` is clear), and the inline path,
hence the constant-stack deferral chains, is unchanged. Regression:
`machine.regression_defer_runs_on_the_executor_when_it_runs_first` (an executor
whose `post()` runs the item on its own thread before returning). `source_machine`
uses the same rule. Executors that may run an item inside `post()` must report
`running_in_this_thread()` for that thread (documented on `executor`).

## Cancellation

Shared state owns a mutex, condition variable and doubly linked intrusive list.
Each node has a pointer to its predecessor's link, so head/middle/tail/random
removal is O(1), independent of how many machines share the token. Removal
updates both neighbors under the mutex before changing the node's mode.

A node cannot be both linked and running. While linked, `previous_` is non-null
and a union holds its successor. When unlinked, `previous_` is null and that
union holds a pointer to a stack-resident invocation record (or null). This
standard C++ union invariant saves one pointer without pointer tagging and gives
O(1) access to running-call bookkeeping too; there is no registration/invocation
list scan on disarm.

Under the mutex a requester removes a node, snapshots fn/ctx and publishes its
stack invocation record, then unlocks before invocation. Foreign disarm waits
until the record is detached; self-disarm nulls the record's node and the node's
record pointer, then returns. On callback return the requester reacquires the
mutex and touches the node only if the record still names it. Self-destruction
or self-rearm therefore cannot leave a dangling access from the old invocation.
A local token reference, declared before the lock, protects state through both
dispatch and unlocking. Empty tokens take no reference-count/lock operations.

Default cancellation wakes parked machines. The driver also checks an
already-requested token when entering park, closing cancellation-before-park.
The check uses conditional delivery so a buffered reply keeps first-wake priority.

*M5 amendment* (`docs/M5-performance.md`). The state's single mutex and list are
now 8 independently spin-locked stripes (a registration's stripe is a hash of its
address; unlinking is still O(1)), with the reference count on its own cache
line; a requester skips stripes whose `occupied` flag is clear, and a foreign
disarm waits for a running callback with `atomic::wait` instead of a condition
variable. Invocation, self-disarm and wait semantics are unchanged. A machine
without an `on_cancel()` hook binds its token at start but registers only at its
first park, *before* publishing `parked`; the four-operation argument below is
unchanged from that point on (the M5 document shows why the later registration
loses no request).

### Formal ordering of park entry against default cancellation

With a cancellable token these four operations are sequentially consistent:

1. Driver publishes `parked` with an SC `fetch_or` on the machine state.
2. Driver queries the token's requested flag with an SC load.
3. Requester sets that flag with an SC exchange before invoking callbacks.
4. Default callback reads the machine state with an SC load before conditional delivery.

They participate in one SC total order. If the driver's query sees false, it is
ordered before the request exchange; that exchange precedes the callback's load,
so the load follows park publication and cannot see an older unparked state.
If the callback sees the state before park publication, the driver's later token
query must see the request. A later state modification that clears `parked` means
a real wake already consumed that park, not a lost cancellation. Thus both sides
cannot observe the old values as the former two-location acquire/release
store-buffering pattern allowed. Wake acceptance is still decided by the
conditional CAS, so the callback cannot inject cancellation into another wait.

Only nonempty tokens access the requested atomic; the driver selects SC park
publication only for those tokens, and callbacks are armed only on real tokens.
Empty-token park publication keeps acquire/release ordering and has no added
SC fence or requested-flag access. SC operations rather than thread fences also
avoid GCC TSan's unsupported-fence instrumentation warning.

Custom hooks run on the requester and must be thread-safe. They choose conditional
delivery for a current park, intentional buffered waking, or cancellation of
external work. Completion
disarms before releasing ownership; no internal lock is held while
invoking hooks, children, work items, or receivers.

## Stress runs

`STEPWRIGHT_STRESS_SCALE` is a positive integer (default 1), propagated through the
Makefile into the container and multiplied into stress iteration counts. The
default race regression uses 20,000 iterations; scale 10 gives 200,000. Each
cross-thread child chain uses 100,000 steps, four persistent completion producers,
and (when bound) a dedicated manual-executor consumer thread. Longer soak runs
can use the same builds with a larger scale; invalid/overflowing values fail.
The two reply-race regressions each run 20,000 iterations times that scale. The
bound test holds the home thread outside the machine until the peer has both
completed the send and woken the reply, forcing every reply to precede processing
the queued child continuation. The unbound test releases separate completion and
reply producers together. Both verify exact-once steps/completion and the reply
value; explicit epoch/callback counters protect test-object reuse and lifetime.
