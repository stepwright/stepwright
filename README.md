# stepwright

> Work in progress: v0.1 is under active development. The API is not stable yet.

stepwright is a header-only C++20 library for writing asynchronous operations as
explicit, zero-allocation state machines. Each step is an ordinary member
function that returns what happens next:

```cpp
struct fetch_and_process : stepwright::machine<fetch_and_process> {
    step begin() { return after<&fetch_and_process::on_fetched>(fetch_); }

    step on_fetched(const stepwright::error& e) {
        if (e) return fail(e);
        process_.emplace(fetch_.data());
        return forward(*process_);
    }

    fetch_op fetch_;
    std::optional<process_op> process_;
};
```

## Build

macOS builds natively; Linux builds run in a pinned Docker container.

```bash
make test          # macOS: configure, build and run the tests
make docker-test   # Linux (in Docker): same, COMPILER=gcc|clang|clang-libcxx
make help          # all targets
```

Supported: macOS (AppleClang) and Linux (GCC 14, Clang 20; x86-64 and arm64).

## Learn and measure

- [`examples/`](examples/README.md): ten runnable programs, from a first machine
  to finding a hang (`make run EXAMPLE=01_first_machine`).
- [`docs/benchmarks.md`](docs/benchmarks.md): published results;
  [`benchmarks/README.md`](benchmarks/README.md): what is measured and how.

## License

MIT, see [LICENSE](LICENSE).
