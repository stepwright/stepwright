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

```bash
make test        # configure, build and run the tests
make help        # all targets
```

## License

MIT, see [LICENSE](LICENSE).
