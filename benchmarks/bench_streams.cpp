#include <stepwright/stepwright.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

namespace {
constexpr stepwright::completion ignored{[](void*, const stepwright::error& e) noexcept {
                                             bool failed = static_cast<bool>(e);
                                             benchmark::DoNotOptimize(failed);
                                         },
                                         nullptr};

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

// --- channel ------------------------------------------------------------------

void BM_channel_send_pull_same_thread(benchmark::State& state) {
    const auto n = state.range(0);
    stepwright::channel<std::int64_t> ch(1024);
    std::int64_t sum = 0;
    for (auto _ : state) {
        for (std::int64_t i = 0; i < n; ++i) {
            ch.try_send(i);
            sum += ch.try_pull().value();
        }
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * n);
}

// Baseline: std::mutex + std::deque, the same operations.
class locked_queue {
public:
    void push(std::int64_t v) {
        bool was_empty = false;
        {
            const std::lock_guard lock(mutex_);
            was_empty = items_.empty();
            items_.push_back(v);
        }
        if (was_empty) {
            ready_.notify_one();
        }
    }
    std::optional<std::int64_t> try_pop() {
        const std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        const auto v = items_.front();
        items_.pop_front();
        return v;
    }
    std::int64_t pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return !items_.empty(); });
        const auto v = items_.front();
        items_.pop_front();
        return v;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::int64_t> items_;
};

void BM_baseline_mutex_deque_same_thread(benchmark::State& state) {
    const auto n = state.range(0);
    locked_queue q;
    std::int64_t sum = 0;
    for (auto _ : state) {
        for (std::int64_t i = 0; i < n; ++i) {
            q.push(i);
            sum += *q.try_pop();
        }
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * n);
}

constexpr std::int64_t spsc_items = 100'000;

// Producer thread try_sends (yielding when full); the consumer thread pulls and
// waits for readiness when the channel is empty.
void BM_channel_spsc_threads(benchmark::State& state) {
    stepwright::channel<std::int64_t> ch(1024);
    std::int64_t sum = 0;
    for (auto _ : state) {
        std::thread producer([&] {
            for (std::int64_t i = 0; i < spsc_items; ++i) {
                while (!ch.try_send(i)) {
                    std::this_thread::yield();
                }
            }
        });
        flag ready;
        for (std::int64_t received = 0; received < spsc_items;) {
            auto next = ch.try_pull();
            if (next.has_item()) {
                sum += next.value();
                ++received;
                continue;
            }
            ch.on_ready(ready.callback(), {});
            ready.wait();
        }
        producer.join();
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * spsc_items);
}

void BM_baseline_mutex_deque_spsc(benchmark::State& state) {
    locked_queue q;
    std::int64_t sum = 0;
    for (auto _ : state) {
        std::thread producer([&] {
            for (std::int64_t i = 0; i < spsc_items; ++i) {
                q.push(i);
            }
        });
        for (std::int64_t received = 0; received < spsc_items; ++received) {
            sum += q.pop();
        }
        producer.join();
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * spsc_items);
}

// Producer machine sending through send_op into a 16-slot channel (waiting while
// full); consumer for_each. Each is bound to a manual executor drained by its own
// thread, so the two sides really run on two threads.
struct sender : stepwright::machine<sender> {
    stepwright::channel<std::int64_t>* ch = nullptr;
    std::int64_t next = 0;
    std::int64_t count = 0;
    std::optional<stepwright::channel<std::int64_t>::send_op> op;
    step begin() {
        if (next == count) {
            ch->close();
            return done();
        }
        op.emplace(*ch, next++);
        return after<&sender::begin>(*op);
    }
};

void drive_until(stepwright::manual_executor& ex, const std::atomic<bool>& done) {
    while (!done.load(std::memory_order_acquire)) {
        if (!ex.run_one()) {
            std::this_thread::yield();
        }
    }
}

void BM_channel_bounded_backpressure(benchmark::State& state) {
    std::int64_t sum = 0;
    for (auto _ : state) {
        stepwright::channel<std::int64_t> ch(16);
        stepwright::manual_executor consumer_home;
        stepwright::manual_executor producer_home;
        sender producer;
        producer.ch = &ch;
        producer.count = spsc_items;
        producer.bind(producer_home);
        auto consumer = stepwright::for_each(ch, [&](std::int64_t v) { sum += v; });
        consumer.bind(consumer_home);
        flag consumed;
        flag produced;
        consumer.start(consumed.callback());
        std::thread thread([&] {
            producer.start(produced.callback());
            drive_until(producer_home, produced.set);
        });
        drive_until(consumer_home, consumed.set);
        thread.join();
        producer_home.run_all();
        consumer_home.run_all();
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * spsc_items);
}

// --- merge --------------------------------------------------------------------

void BM_merge_pull(benchmark::State& state) {
    auto make = [] {
        return stepwright::generate(
            [n = std::int64_t{0}]() mutable -> std::optional<std::int64_t> { return n++; });
    };
    auto s0 = make();
    auto s1 = make();
    auto s2 = make();
    auto s3 = make();
    auto s4 = make();
    auto s5 = make();
    auto s6 = make();
    auto s7 = make();
    stepwright::merge m(s0, s1, s2, s3, s4, s5, s6, s7);
    std::int64_t sum = 0;
    for (auto _ : state) {
        for (int i = 0; i < 1000; ++i) {
            sum += m.try_pull().value();
        }
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * 1000);
}

// --- broadcast ----------------------------------------------------------------

// One publish followed by every subscriber pulling it; per subscriber delivery.
void BM_broadcast_publish(benchmark::State& state) {
    const auto subscribers = static_cast<std::size_t>(state.range(0));
    stepwright::broadcast<std::int64_t> b(64);
    std::vector<std::unique_ptr<stepwright::broadcast<std::int64_t>::subscription>> subs;
    for (std::size_t i = 0; i < subscribers; ++i) {
        subs.push_back(std::make_unique<stepwright::broadcast<std::int64_t>::subscription>(b));
    }
    std::int64_t sum = 0;
    std::int64_t next = 0;
    for (auto _ : state) {
        b.try_publish(next++);
        for (auto& s : subs) {
            sum += s->try_pull().value();
        }
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// --- source_machine / for_each ------------------------------------------------

struct counter : stepwright::source_machine<counter, std::int64_t> {
    std::int64_t next = 0;
    std::int64_t limit = 0;
    step begin() { return next == limit ? end() : emit<&counter::begin>(next++); }
};

void BM_source_machine_emit(benchmark::State& state) {
    const auto n = state.range(0);
    std::int64_t sum = 0;
    for (auto _ : state) {
        counter c;
        c.limit = n;
        for (auto next = c.try_pull(); next.has_item(); next = c.try_pull()) {
            sum += next.value();
        }
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * n);
}

void BM_for_each_generate(benchmark::State& state) {
    const auto n = state.range(0);
    std::int64_t sum = 0;
    for (auto _ : state) {
        auto g =
            stepwright::generate([i = std::int64_t{0}, n]() mutable -> std::optional<std::int64_t> {
                if (i == n) {
                    return std::nullopt;
                }
                return i++;
            });
        auto op = stepwright::for_each(g, [&](std::int64_t v) { sum += v; });
        op.start(ignored);
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations() * n);
}

void BM_baseline_vector_loop(benchmark::State& state) {
    std::vector<std::int64_t> values(static_cast<std::size_t>(state.range(0)));
    std::iota(values.begin(), values.end(), 0);
    std::int64_t sum = 0;
    for (auto _ : state) {
        for (const auto v : values) {
            benchmark::DoNotOptimize(sum += v);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
} // namespace

BENCHMARK(BM_channel_send_pull_same_thread)->Arg(1000);
BENCHMARK(BM_baseline_mutex_deque_same_thread)->Arg(1000);
BENCHMARK(BM_channel_spsc_threads)->UseRealTime();
BENCHMARK(BM_baseline_mutex_deque_spsc)->UseRealTime();
BENCHMARK(BM_channel_bounded_backpressure)->UseRealTime();
BENCHMARK(BM_merge_pull)->Arg(8);
BENCHMARK(BM_broadcast_publish)->Arg(1)->Arg(16)->Arg(1000);
BENCHMARK(BM_source_machine_emit)->Arg(1000);
BENCHMARK(BM_for_each_generate)->Arg(1000);
BENCHMARK(BM_baseline_vector_loop)->Arg(1000);
