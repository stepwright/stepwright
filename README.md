# stepwright

**Write asynchronous operations as explicit, inspectable C++20 state machines —
without heap allocation per step or wait.** Header-only, MIT-licensed, for macOS
and Linux.

An operation is a class. A step returns what should happen next; a small driver
loop does it. State and children are ordinary members, not hidden frames:

```cpp
#include <stepwright/stepwright.hpp>
#include <chrono>
#include <cstdio>
struct reminder : stepwright::machine<reminder> {
    stepwright::delay pause;
    explicit reminder(stepwright::timer_service& timers)
        : pause(timers, std::chrono::milliseconds(25)) {}
    step begin() { return after<&reminder::announce>(pause); }
    step announce() { std::puts("ready"); return done(); }
};
int main() {
    stepwright::testing::virtual_executor clock;
    reminder op(clock);
    return clock.run(op) ? 1 : 0;
}
```

Build this as `c++ -std=c++20 -pthread -Iinclude demo.cpp -o demo`, or link the
CMake interface target below. The [guide](docs/guide.md) starts with the task and
lifetime contracts; [ten runnable examples](examples/README.md) show real timers,
thread pools, cancellation, backpressure and deterministic race replay.
Virtual time makes this demo instant; for real timers use a `steady_timer_service`
and `sync_wait(op)` from main, never from a step or executor worker.

## Learn by example

[Seven side-by-side programs](examples/learn/README.md) compare plain C++ with
machines: factorial, Fibonacci, parallel sorting and summation, deep chains,
retry and deadlines. Prefer plain loops for synchronous numeric work; use
stepwright when work needs to wait, share threads or support cancellation.

## Why stepwright?

- **Explicit state.** Every suspension is a named step. Inspect members and the
  next step in a debugger; opt into a live task registry to diagnose hangs.
- **Predictable successful hot paths.** Embedded waiter/work nodes, static step
  dispatch and constant-stack driver loops, including synchronous child chains.
- **Structured work.** Tokens propagate to children. Joins and deadlines wait for
  all started work to finish; errors are values with machine-name context.
- **Useful building blocks.** Blocking and virtual-time run helpers, container
  joins, sequences, cancellable retry/backoff, reusable waits, events, shared
  tasks, async mutexes, timers, pull streams and a work-stealing pool.
- **Testable scheduling.** Manual time and a seeded virtual executor explore and
  replay interleavings without real sleeps.

### The trade-offs

| Approach | Prefer it when… | What stepwright trades |
|---|---|---|
| C++20 coroutines | You want natural sequential syntax and compatible awaiters. Visible child frames may be elided and be faster. | Named steps and member-owned state make storage/lifetime explicit, but require more code. No coroutine support or frame elision is required. |
| Senders/receivers | You need a general execution/composition vocabulary and its ecosystem. | A smaller model with one task protocol, not a P2300 implementation or adapter. |
| Callbacks | Your operation is small and ownership is already clear. | A driver loop, consistent cancellation and errors instead of ad hoc chains; some bookkeeping over a bare call. |

Zero-allocation claims concern successful **steps and waits**, not everything a
program does: pools, source state, join storage and buffers allocate at construction;
errors, diagnostics, callable convenience posts and unbounded buffer growth can
allocate. See [performance guidance](docs/guide.md#performance-guidance).

## Install

CMake 3.21 or newer is needed for CMake integration; the library needs only C++20
and threads by default. Developer dependencies are not fetched for subprojects.

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(stepwright
    GIT_REPOSITORY https://github.com/stepwright/stepwright.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(stepwright)
target_link_libraries(my_app PRIVATE stepwright::stepwright)
```

`v0.1.0` is the first release tag.
`my_app` denotes your existing executable/library target.

### Installed package

```bash
cmake -S . -B build/install -DCMAKE_BUILD_TYPE=Release \
  -DSTEPWRIGHT_BUILD_TESTS=OFF -DSTEPWRIGHT_BUILD_EXAMPLES=OFF -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF
cmake --install build/install --prefix "$PWD/build/prefix"
# Configure your application with -DCMAKE_PREFIX_PATH="$PWD/build/prefix"
```

```cmake
find_package(stepwright 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE stepwright::stepwright)
```

The package exports headers, C++20 and `Threads::Threads`, not developer warning
or sanitizer flags. While major version is zero, CMake compatibility is confined
to the same minor version. A relocatable `stepwright.pc` is installed under
`lib/pkgconfig` (or your `CMAKE_INSTALL_LIBDIR`): add that directory to
`PKG_CONFIG_PATH` and use `pkg-config --cflags --libs stepwright`.

### Vendored source

```cmake
add_subdirectory(external/stepwright)
target_link_libraries(my_app PRIVATE stepwright::stepwright)
```

Alternatively, copy `include/stepwright/` and add its parent to your include path.
Include `<stepwright/stepwright.hpp>` or individual self-contained headers. Only opt-in
logging needs an external package (`spdlog`).

## Supported platforms

| Platform | Support floor | Also exercised |
|---|---|---|
| macOS arm64 | AppleClang 16 | Native Release, Debug and sanitizers |
| Linux arm64 and x86-64 | GCC 13, Clang 18, with a C++20 standard library | GCC 14, Clang 20 with libstdc++ and libc++ |

Windows is not supported. CI runs Linux in the pinned development image and macOS
natively; x86-64 Linux is also tested under Docker emulation on Apple silicon
(except TSan, which requires a native runner). There is no stable binary ABI or
promise of source compatibility across 0.x minor versions.

## Build, test and documentation

```bash
export PATH="/opt/homebrew/bin:$PATH"  # macOS, if CMake is installed there
make test                            # native Release + tests + examples
make debug
make asan                            # ASan + UBSan
make tsan
COMPILER=gcc make docker-test        # Linux, no host tool installation
COMPILER=gcc13 make docker-test      # support floor (or clang18)
make consumer                        # native integration tests
make docker-consumer
make format-check                    # pinned clang-format, in Docker
make docs                            # Doxygen HTML, in Docker
```

The API reference is generated in the Docker build volume at `/work/build/docs/html`.
To copy it out locally, create `build/api`, then run
`docker run --rm -v stepwright-build-host:/work/build -v "$PWD/build/api":/out ubuntu:24.04 cp -r /work/build/docs/html /out/`.
Use `make help` and [CONTRIBUTING.md](CONTRIBUTING.md) for the full quality matrix.

## Performance

The [published benchmark results](docs/benchmarks.md) include engine steps,
primitives, streams, threaded workloads, baseline pools and coroutine comparisons,
with flags, machine context, medians and variation. The [method](benchmarks/README.md)
explains what is counted, where frames allocate and where scheduling dominates.
In this run a `then` step is 1.92 ns natively and 2.62 ns on Linux/GCC; an `after`
on a synchronous child is 4.05 / 5.27 ns. With useful leaf work, the eight-worker
fan-out is 7.3× / 6.6× faster than its one-worker reference.
They are measurements, not a guarantee for your workload; stepwright does not always
beat a compiler-elided coroutine or a simple hand-written callback.

## Read more

- [User guide and FAQ](docs/guide.md) · [examples](examples/README.md)
- [Overview](docs/overview.md) · [engine](docs/design/engine.md) ·
  [primitives](docs/design/primitives.md) · [streams](docs/design/streams.md) ·
  [runtime](docs/design/runtime.md) · [performance design](docs/design/performance.md)
  · [composition correctness](docs/design/composition.md)
- [Design provenance](docs/provenance.md) · [changelog](CHANGELOG.md)
- [Contributing](CONTRIBUTING.md) · [code of conduct](CODE_OF_CONDUCT.md) ·
  [security policy](SECURITY.md)

## License

[MIT](LICENSE). Contributions use the same license and DCO sign-off.
