#include <stepwright/stepwright.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace {
constexpr stepwright::completion ignored{[](void*, const stepwright::error& e) noexcept {
                                             bool failed = static_cast<bool>(e);
                                             benchmark::DoNotOptimize(failed);
                                         },
                                         nullptr};

struct sync_child : stepwright::task {
    void on_start(stepwright::completion done, stepwright::cancel_token) override { done({}); }
};
struct async_child : stepwright::task, stepwright::work_item {
    stepwright::manual_executor* ex = nullptr;
    stepwright::completion done;
    async_child() {
        run = [](stepwright::work_item* item) noexcept {
            if (!item) {
                std::terminate();
            }
            const auto receiver = static_cast<async_child*>(item)->done;
            receiver({});
        };
    }
    void on_start(stepwright::completion receiver, stepwright::cancel_token) override {
        done = receiver;
        ex->post(static_cast<stepwright::work_item&>(*this));
    }
};
template <int Mode>
struct chain : stepwright::machine<chain<Mode>> {
    using base = stepwright::machine<chain<Mode>>;
    using typename base::step;
    std::int64_t remaining = 0;
    sync_child child;
    async_child async;
    step begin() {
        benchmark::DoNotOptimize(remaining);
        if (remaining-- == 0) {
            return this->done();
        }
        if constexpr (Mode == 0) {
            return this->template then<&chain::begin>();
        }
        if constexpr (Mode == 1) {
            return this->template after<&chain::begin>(child);
        }
        if constexpr (Mode == 2) {
            return this->template after<&chain::begin>(async);
        }
        if constexpr (Mode == 3) {
            this->wake();
            return this->template park<&chain::begin>();
        }
    }
};
template <int Mode>
void run_chain(benchmark::State& state) {
    chain<Mode> op;
    stepwright::manual_executor ex;
    op.async.ex = &ex;
    for (auto _ : state) {
        op.remaining = state.range(0);
        op.start(ignored);
        if constexpr (Mode == 2) {
            ex.run_all();
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
void BM_then_chain(benchmark::State& state) {
    run_chain<0>(state);
}
void BM_after_sync_child(benchmark::State& state) {
    run_chain<1>(state);
}
void BM_after_async_child_manual(benchmark::State& state) {
    run_chain<2>(state);
}
void BM_park_wake_same_thread(benchmark::State& state) {
    run_chain<3>(state);
}

void BM_park_wake_ping_pong_threads(benchmark::State& state) {
    struct ping_pong : stepwright::machine<ping_pong> {
        bool stop = false;
        step begin() { return park<&ping_pong::resumed>(); }
        step resumed() { return stop ? done() : park<&ping_pong::resumed>(); }
    } op;
    std::atomic<int> request{0}, acknowledged{0};
    op.start(ignored);
    std::thread worker([&] {
        for (;;) {
            while (request.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }
            const bool stop = op.stop;
            op.wake();
            request.store(0, std::memory_order_relaxed);
            acknowledged.store(1, std::memory_order_release);
            if (stop) {
                return;
            }
        }
    });
    for (auto _ : state) {
        request.store(1, std::memory_order_release);
        while (acknowledged.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        acknowledged.store(0, std::memory_order_relaxed);
    }
    op.stop = true;
    request.store(1, std::memory_order_release);
    worker.join();
    state.SetItemsProcessed(state.iterations());
}

void BM_error_propagation_depth(benchmark::State& state) {
    struct failure : stepwright::machine<failure> {
        step begin() {
            return fail(stepwright::error::make(stepwright::errc::closed, "benchmark"));
        }
    } leaf;
    struct layer : stepwright::machine<layer> {
        stepwright::task* child = nullptr;
        step begin() { return forward(*child); }
    };
    std::array<layer, 8> layers;
    stepwright::task* root = &leaf;
    for (auto& item : layers) {
        item.child = root;
        root = &item;
    }
    for (auto _ : state) {
        root->start(ignored);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

struct fnptr_state {
    std::int64_t remaining;
    using fn_type = bool (*)(fnptr_state&);
    static bool next(fnptr_state& self) {
        benchmark::DoNotOptimize(self.remaining);
        return self.remaining-- != 0;
    }
};
void BM_baseline_fnptr_chain(benchmark::State& state) {
    fnptr_state::fn_type call = &fnptr_state::next;
    benchmark::DoNotOptimize(call);
    for (auto _ : state) {
        fnptr_state op{state.range(0)};
        while (call(op)) {
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
void BM_baseline_std_function_chain(benchmark::State& state) {
    std::function<bool(fnptr_state&)> call = &fnptr_state::next;
    for (auto _ : state) {
        fnptr_state op{state.range(0)};
        while (call(op)) {
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
void BM_shared_token_complete(benchmark::State& state) {
    struct parked : stepwright::machine<parked> {
        step begin() { return park<&parked::finish>(); }
        step finish() { return done(); }
    };
    const auto size = static_cast<std::size_t>(state.range(0));
    stepwright::cancel_source source;
    auto ops = std::make_unique<parked[]>(size);
    for (auto _ : state) {
        state.PauseTiming();
        for (std::size_t i = 0; i < size; ++i) {
            ops[i].start(ignored, source.token());
        }
        state.ResumeTiming();
        for (std::size_t i = 0; i < size; ++i) {
            ops[i].wake();
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// --- C++20 coroutine baseline ---------------------------------------------------------
//
// A minimal lazy coroutine task, written as a competent user would write one
// without a library: the frame starts suspended, co_await stores the awaiting
// coroutine and transfers to the child (symmetric transfer, so chains of
// synchronously completing children use constant stack), and final_suspend
// transfers back to the awaiting coroutine. The result is a stepwright::error, as
// for machines; exceptions terminate. Each call creates a frame; the promise
// counts frame allocations so the benchmarks can report whether the compiler
// elided them (benchmarks/README.md).
std::int64_t coro_frames = 0; // single-threaded benchmarks only

struct coro_task {
    struct promise_type {
        stepwright::error result;
        std::coroutine_handle<> continuation = std::noop_coroutine();
        static void* operator new(std::size_t size) {
            ++coro_frames;
            return ::operator new(size);
        }
        static void operator delete(void* frame, std::size_t size) noexcept {
            ::operator delete(frame, size);
        }
        coro_task get_return_object() noexcept {
            return coro_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct final_awaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<promise_type> self) const noexcept {
                return self.promise().continuation;
            }
            void await_resume() const noexcept {}
        };
        final_awaiter final_suspend() noexcept { return {}; }
        void return_value(stepwright::error e) noexcept { result = std::move(e); }
        [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
    };

    explicit coro_task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
    coro_task(coro_task&& other) noexcept : handle(std::exchange(other.handle, {})) {}
    coro_task(const coro_task&) = delete;
    coro_task& operator=(const coro_task&) = delete;
    coro_task& operator=(coro_task&&) = delete;
    ~coro_task() {
        if (handle) {
            handle.destroy();
        }
    }

    // Awaiting a task runs it to completion and yields its result.
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle.promise().continuation = awaiting;
        return handle;
    }
    stepwright::error await_resume() noexcept { return std::move(handle.promise().result); }
    // Run a root task on this thread (all benchmarks here complete synchronously).
    stepwright::error run() {
        handle.resume();
        return std::move(handle.promise().result);
    }

    std::coroutine_handle<promise_type> handle;
};

// Same work per step as chain<0>: the coroutine body is the chain.
coro_task coro_then_chain(std::int64_t remaining) {
    for (;;) {
        benchmark::DoNotOptimize(remaining);
        if (remaining-- == 0) {
            co_return stepwright::error{};
        }
    }
}
coro_task coro_sync_child() {
    co_return stepwright::error{};
}
// The same child where the compiler cannot see the call (as for a child defined
// in another translation unit, or reached through an interface): no elision.
#if __has_cpp_attribute(gnu::noinline)
[[gnu::noinline]]
#endif
coro_task coro_opaque_sync_child() {
    co_return stepwright::error{};
}
// Same work per step as chain<1>: await a synchronously completing child, and
// stop on its failure (as a no-argument machine step does).
template <coro_task (*Child)()>
coro_task coro_after_sync_child(std::int64_t remaining) {
    for (;;) {
        benchmark::DoNotOptimize(remaining);
        if (remaining-- == 0) {
            co_return stepwright::error{};
        }
        stepwright::error e = co_await Child();
        if (e) {
            co_return e;
        }
    }
}
template <class Make>
void run_coro(benchmark::State& state, Make make) {
    coro_frames = 0;
    for (auto _ : state) {
        coro_task root = make(state.range(0));
        const stepwright::error e = root.run();
        bool failed = static_cast<bool>(e);
        benchmark::DoNotOptimize(failed);
    }
    const auto items = state.iterations() * state.range(0);
    state.SetItemsProcessed(items);
    state.counters["frames_per_item"] =
        static_cast<double>(coro_frames) / static_cast<double>(items == 0 ? 1 : items);
}
void BM_coro_then_chain(benchmark::State& state) {
    run_coro(state, coro_then_chain);
}
void BM_coro_after_sync_child(benchmark::State& state) {
    run_coro(state, coro_after_sync_child<&coro_sync_child>);
}
void BM_coro_after_opaque_sync_child(benchmark::State& state) {
    run_coro(state, coro_after_sync_child<&coro_opaque_sync_child>);
}
} // namespace

BENCHMARK(BM_then_chain)->Arg(1000);
BENCHMARK(BM_after_sync_child)->Arg(1000);
BENCHMARK(BM_after_async_child_manual)->Arg(1000);
BENCHMARK(BM_park_wake_same_thread)->Arg(1000);
BENCHMARK(BM_park_wake_ping_pong_threads)->UseRealTime();
BENCHMARK(BM_error_propagation_depth)->Arg(8);
BENCHMARK(BM_baseline_fnptr_chain)->Arg(1000);
BENCHMARK(BM_baseline_std_function_chain)->Arg(1000);
BENCHMARK(BM_coro_then_chain)->Arg(1000);
BENCHMARK(BM_coro_after_sync_child)->Arg(1000);
BENCHMARK(BM_coro_after_opaque_sync_child)->Arg(1000);
BENCHMARK(BM_shared_token_complete)->Arg(1000)->Arg(10000)->Arg(100000);
