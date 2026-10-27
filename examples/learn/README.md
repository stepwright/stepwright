# Learn by example

Each complete program puts a real plain C++ implementation beside a stepwright
implementation, checks agreement and prints deterministic results. There are no
performance claims: a step costs a few nanoseconds; a tight loop iteration costs
less. The point is control over waiting, scheduling, lifetime and cancellation.

| Program / problem | Plain approach | stepwright approach | Use stepwright when… |
|---|---|---|---|
| [factorial](factorial.cpp): million-factor factorial modulo a prime | Numeric loop | `then` loop, batch `defer`, progress reporter and cancelled second run | Long work must share a thread fairly and stop on request. |
| [fibonacci](fibonacci.cpp): values through the first exceeding a million | Precompute a vector | Lazy `source_machine`, early consumer stop, timer-paced variant | A producer may be unbounded, slow or backpressured. |
| [merge_sort](merge_sort.cpp): a million seeded integers | `std::sort` | Prebuilt tree, `when_all`, four-worker pool and reusable scratch | Independent subproblems merit bounded parallelism and structured lifetime. |
| [parallel_sum](parallel_sum.cpp): a million integers | `std::accumulate` | Container join over chunks; validation failure cancels waiting siblings | Map/reduce chunks may need I/O, failure propagation or cancellation. |
| [deep_recursion](deep_recursion.cpp): ten-million-node recurrence | Recursion at a safe small depth | Sequential synchronous child visits in a constant-stack driver | Traversal may be deep or become asynchronous. Nested parent starts are not automatically flattened. |
| [retry_download](retry_download.cpp): a server fails twice | Loop and blocking `sleep_for` | `retry`, exact virtual timestamps, `sequence` and cancelled backoff | Waiting must release threads and timing must be testable. |
| [timeout_computation](timeout_computation.cpp): computation budget | Application-polled flag | Batch deferral, explicit modeled cost and reusable deadline | You need cooperative deadlines across work and its waits. |

## Running

`make test` builds and checks all seven as `example_learn_<name>` CTest tests.
The executables are `build/native-release/examples/learn_<name>`; for example
`make run EXAMPLE=learn_factorial`. Linux uses `make docker-run` and the same name.
No example prints wall timings, addresses or thread identities.

## When not to use stepwright

- Simple synchronous code that never waits: an ordinary function is clearer.
- Hot numeric loops: batch useful work rather than wrapping each arithmetic operation.
- Small finite sequences already in memory: a vector and a loop may be all you need.
- Work requiring forced preemption: cancellation is cooperative, not a thread kill.

Virtual time advances only when nothing is ready. The computation-budget example
therefore models a batch's time with an explicit delay; it does not pretend a
virtual executor measures CPU time. Merge sort allocates its tree/scratch before
starting; no per-task allocation occurs while sorting. The deep traversal reuses
one child at a time rather than allocating ten million live machine frames.
