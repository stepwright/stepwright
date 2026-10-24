#include <stepwright/stepwright.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct flag {
    std::atomic<bool> set{false};
    void finish(const stepwright::error& /*e*/) noexcept {
        set.store(true, std::memory_order_release);
    }
    stepwright::completion callback() noexcept {
        return stepwright::completion::bind<&flag::finish>(this);
    }
    void wait() noexcept {
        while (!set.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        set.store(false, std::memory_order_relaxed);
    }
};

// Baseline: the textbook pool. One std::mutex, one condition variable, a
// std::deque<std::function<void()>>; notify_one per post. The pending count is
// updated in the same critical section as the next pop (one lock per item).
class baseline_pool {
public:
    explicit baseline_pool(std::size_t threads) {
        for (std::size_t i = 0; i < threads; ++i) {
            threads_.emplace_back([this] { work(); });
        }
    }
    baseline_pool(const baseline_pool&) = delete;
    baseline_pool& operator=(const baseline_pool&) = delete;
    ~baseline_pool() {
        {
            const std::lock_guard lock(mutex_);
            stop_ = true;
        }
        ready_.notify_all();
        for (auto& t : threads_) {
            t.join();
        }
    }
    void post(std::function<void()> fn) {
        {
            const std::lock_guard lock(mutex_);
            queue_.push_back(std::move(fn));
            ++pending_;
        }
        ready_.notify_one();
    }
    void wait_idle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [&] { return pending_ == 0; });
    }

private:
    void work() {
        bool ran = false;
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock lock(mutex_);
                if (ran && --pending_ == 0) {
                    idle_.notify_all();
                }
                ready_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    return;
                }
                fn = std::move(queue_.front());
                queue_.pop_front();
            }
            fn();
            ran = true;
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> queue_;
    std::size_t pending_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

constexpr std::size_t outside_items = 100'000;

struct tiny : stepwright::work_item {
    tiny() noexcept {
        run = [](stepwright::work_item* item) noexcept { benchmark::DoNotOptimize(item); };
    }
};

// --- posted from outside -----------------------------------------------------------

void BM_pool_post_run(benchmark::State& state) {
    stepwright::thread_pool pool(static_cast<std::size_t>(state.range(0)));
    auto items = std::make_unique<tiny[]>(outside_items);
    for (auto _ : state) {
        for (std::size_t i = 0; i < outside_items; ++i) {
            pool.post(items[i]);
        }
        pool.wait_idle();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(outside_items));
}

void BM_baseline_pool_post_run(benchmark::State& state) {
    baseline_pool pool(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        for (std::size_t i = 0; i < outside_items; ++i) {
            pool.post([i]() mutable { benchmark::DoNotOptimize(i); });
        }
        pool.wait_idle();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(outside_items));
}

// --- posted from workers: one chain per thread, each item posting the next ----------

constexpr long chain_length = 200'000;

// Padded: chains are written by different workers on every item.
struct alignas(128) chain : stepwright::work_item {
    stepwright::thread_pool* pool = nullptr;
    long remaining = 0;
    chain() noexcept {
        run = [](stepwright::work_item* item) noexcept {
            auto& self = *static_cast<chain*>(item);
            if (--self.remaining > 0) {
                self.pool->post(self);
            }
        };
    }
};

void BM_pool_post_from_worker(benchmark::State& state) {
    const auto threads = static_cast<std::size_t>(state.range(0));
    stepwright::thread_pool pool(threads);
    std::vector<chain> chains(threads);
    for (auto _ : state) {
        for (auto& c : chains) {
            c.pool = &pool;
            c.remaining = chain_length;
            pool.post(c);
        }
        pool.wait_idle();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(threads) * chain_length);
}

void baseline_chain_step(baseline_pool& pool, long remaining) {
    if (--remaining > 0) {
        pool.post([&pool, remaining] { baseline_chain_step(pool, remaining); });
    }
}

void BM_baseline_pool_post_from_worker(benchmark::State& state) {
    const auto threads = static_cast<std::size_t>(state.range(0));
    baseline_pool pool(threads);
    for (auto _ : state) {
        for (std::size_t i = 0; i < threads; ++i) {
            pool.post([&pool] { baseline_chain_step(pool, chain_length); });
        }
        pool.wait_idle();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(threads) * chain_length);
}

// --- ping-pong between two machines bound to the pool -------------------------------
// Each side defers (one pool post + run) and then wakes the other, which is parked.
// One iteration of the benchmark loop is 10,000 round trips.

constexpr int round_trips = 10'000;

struct pong_side;
struct ping_side : stepwright::machine<ping_side> {
    pong_side* peer = nullptr;
    int remaining = 0;
    step begin() { return defer<&ping_side::send>(); }
    step send();
    step on_pong() { return defer<&ping_side::send>(); }
};
struct pong_side : stepwright::machine<pong_side> {
    ping_side* peer = nullptr;
    bool stop = false;
    step begin() { return park<&pong_side::on_ping>(); }
    step on_ping() {
        if (stop) {
            return done();
        }
        return defer<&pong_side::send>();
    }
    step send() {
        peer->wake();
        return park<&pong_side::on_ping>();
    }
};
ping_side::step ping_side::send() {
    if (remaining-- == 0) {
        peer->stop = true;
        peer->wake();
        return done();
    }
    peer->wake();
    return park<&ping_side::on_pong>();
}

void BM_pool_ping_pong(benchmark::State& state) {
    stepwright::thread_pool pool(2);
    ping_side ping;
    pong_side pong;
    ping.peer = &pong;
    pong.peer = &ping;
    ping.bind(pool);
    pong.bind(pool);
    flag ping_done;
    flag pong_done;
    for (auto _ : state) {
        ping.remaining = round_trips;
        pong.stop = false;
        pong.start(pong_done.callback());
        ping.start(ping_done.callback());
        ping_done.wait();
        pong_done.wait();
    }
    pool.wait_idle();
    state.SetItemsProcessed(state.iterations() * round_trips);
}

// --- fan-out / fan-in ---------------------------------------------------------------

struct leaf : stepwright::machine<leaf> {
    step begin() { return defer<&leaf::finish_up>(); }
    step finish_up() { return done(); }
};
struct launcher : stepwright::machine<launcher> {
    stepwright::task* target = nullptr;
    step begin() { return defer<&launcher::go>(); }
    step go() { return forward(*target); }
};

// when_all_range of `count` machines bound to the pool, started from a worker; each
// machine defers once. Per machine.
void fan_out_fan_in(benchmark::State& state, std::size_t threads) {
    const auto count = static_cast<std::size_t>(state.range(0));
    stepwright::thread_pool pool(threads);
    std::vector<leaf> leaves(count);
    std::vector<stepwright::task*> children;
    for (auto& l : leaves) {
        l.bind(pool);
        children.push_back(&l);
    }
    stepwright::when_all_range all(children);
    launcher start;
    start.target = &all;
    start.bind(pool);
    flag done;
    for (auto _ : state) {
        start.start(done.callback());
        done.wait();
    }
    pool.wait_idle();
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}
void BM_pool_fan_out_fan_in(benchmark::State& state) {
    fan_out_fan_in(state, 8);
}
// Reference: the same work on one worker (no stealing, no contention).
void BM_pool_fan_out_fan_in_one_thread(benchmark::State& state) {
    fan_out_fan_in(state, 1);
}

// --- virtual executor ---------------------------------------------------------------

void BM_virtual_executor_post_run(benchmark::State& state) {
    constexpr std::size_t n = 1'000;
    stepwright::testing::virtual_executor ex;
    auto items = std::make_unique<tiny[]>(n);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            ex.post(items[i]);
        }
        ex.run_until_idle();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
} // namespace

BENCHMARK(BM_pool_post_run)->Arg(1)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_baseline_pool_post_run)->Arg(1)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_pool_post_from_worker)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_baseline_pool_post_from_worker)->Arg(8)->UseRealTime();
BENCHMARK(BM_pool_ping_pong)->UseRealTime();
BENCHMARK(BM_pool_fan_out_fan_in)->Arg(10'000)->UseRealTime();
BENCHMARK(BM_pool_fan_out_fan_in_one_thread)->Arg(10'000)->UseRealTime();
BENCHMARK(BM_virtual_executor_post_run);
