# Design provenance

stepwright was designed and implemented independently. Its implementation is original
MIT-licensed code; the references below explain the public ideas and vocabulary
that informed it, not code copied into the library. Contributors must have the
right to contribute their changes under MIT and cite relevant prior art here.

| Component | Public influences |
|---|---|
| Machine engine | Classic trampolined/driver-loop interpreters for constant-stack continuation chains; Rust's explicit `Future::poll` state-machine model. [Boost.Asio composed operations](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/composition.html) informed the task/completion/executor vocabulary. |
| Cancellation and joins | [P2300](https://wg21.link/p2300) and libunifex's structured `when_all`/stop-token model. Intrusive registration and stack-resident invocation records are independently designed. The request/activation flag handshake uses the sequential-consistency principle of Dekker's algorithm, described in Dijkstra, “Solution of a Problem in Concurrent Programming Control” (CACM, 1965). |
| Wait primitives | [cppcoro](https://github.com/lewissbaker/cppcoro)'s public descriptions of `async_manual_reset_event`, `shared_task` and `async_mutex` informed naming and semantics. The publish/arm/resolve protocol and intrusive hand-off trampoline are independent designs. |
| Cancellation stripes and join shards | Herlihy and Shavit, *The Art of Multiprocessor Programming* (2008): lock striping and distributed counters. Knuth, *The Art of Computer Programming*, vol. 3, §6.4: multiplicative/Fibonacci hashing for stripe selection. |
| Timers | Fredman, Sedgewick, Sleator and Tarjan, “The Pairing Heap: A New Form of Self-Adjusting Heap” (Algorithmica, 1986). The sequence tie-break, firing records and structured cancellation are independent designs. |
| Pull streams | Rust's stream polling/readiness model; [Go channels](https://go.dev/ref/spec#Channel_types) for bounded queues and close/drain semantics. The readiness-round merge and per-slot-reader-count broadcast ring are independent designs. |
| Thread pool | Blumofe and Leiserson, “Scheduling Multithreaded Computations by Work Stealing” (JACM, 1999); Vyukov, [“Scalable Go Scheduler Design Doc”](https://golang.org/s/go11sched) (2012), for periodic global checks and delayed single-item stealing; Lerche, [“Making the Tokio scheduler 10x faster”](https://tokio.rs/blog/2019-10-scheduler), for bounded local queues, batching, search caps and search hand-over. The guard, conditional sleep transition, idle counters and liveness proofs are independent designs. |
| Deterministic executor | Steele, Lea and Flood, “Fast Splittable Pseudorandom Number Generators” (OOPSLA, 2014), for splitmix64. Virtual time, ready-set arbitration and integration with the intrusive timer protocol are independent designs. |
| Debug registry | Intrusive lists and locked snapshots; compile-time type/step spellings use compiler-provided function-signature strings, not RTTI. |
| Benchmarks | [Google Benchmark user guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md), for repetitions, aggregate statistics, JSON and real-time measurement. The independently written coroutine baseline follows [P0913R1](https://wg21.link/p0913r1) and Baker's [symmetric-transfer explanation](https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer). |
| Packaging and documentation tooling | CMake's documented interface-target/install/export model, GNUInstallDirs and CMakePackageConfigHelpers; Doxygen's documented warning and HTML-generation interface; Git's NUL-delimited tracked-file protocol. Consumer projects, snippet extraction and the fail-closed host/container reference scanner are written for this repository. |

External development dependencies retain their own licenses: GoogleTest,
Google Benchmark and optional spdlog. None is required by the default installed
library; stepwright's production dependency is the C++20 standard library and threads.
