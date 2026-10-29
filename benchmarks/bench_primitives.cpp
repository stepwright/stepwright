#include <stepwright/stepwright.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

void BM_event_set_wait(benchmark::State& state) {
    const auto waiters = static_cast<std::size_t>(state.range(0));
    stepwright::event ev;
    std::vector<std::unique_ptr<stepwright::event::waiter>> pool;
    pool.reserve(waiters);
    for (std::size_t i = 0; i < waiters; ++i) {
        pool.push_back(std::make_unique<stepwright::event::waiter>(ev));
    }
    for (auto _ : state) {
        for (auto& waiter : pool) {
            waiter->start(ignored);
        }
        ev.set();
        ev.reset();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_mutex_uncontended(benchmark::State& state) {
    stepwright::async_mutex m;
    auto op = m.lock();
    for (auto _ : state) {
        op.start(ignored);
        m.unlock();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_baseline_std_mutex_uncontended(benchmark::State& state) {
    std::mutex m;
    for (auto _ : state) {
        m.lock();
        benchmark::ClobberMemory();
        m.unlock();
    }
    state.SetItemsProcessed(state.iterations());
}

struct chain_locker : stepwright::machine<chain_locker> {
    explicit chain_locker(stepwright::async_mutex& m) : mutex(&m), op(m) {}
    stepwright::async_mutex* mutex;
    stepwright::async_mutex::lock_op op;
    std::int64_t* remaining = nullptr;
    step begin() { return after<&chain_locker::locked>(op); }
    step locked() {
        const bool more = (*remaining)-- > 0;
        mutex->unlock();
        return more ? then<&chain_locker::begin>() : done();
    }
};

void BM_mutex_handoff_chain(benchmark::State& state) {
    stepwright::async_mutex m;
    std::int64_t remaining = 0;
    std::vector<std::unique_ptr<chain_locker>> lockers;
    for (int i = 0; i < 4; ++i) {
        lockers.push_back(std::make_unique<chain_locker>(m));
        lockers.back()->remaining = &remaining;
    }
    for (auto _ : state) {
        remaining = state.range(0);
        m.try_lock();
        for (auto& locker : lockers) {
            locker->start(ignored);
        }
        m.unlock(); // the whole chain runs here, through the trampoline
    }
    // Each hand-off grants the lock to the next queued machine.
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_mutex_contended_threads(benchmark::State& state) {
    const auto threads = static_cast<int>(state.range(0));
    constexpr int per_thread = 1000;
    stepwright::async_mutex m;
    std::atomic<int> epoch{0};
    std::atomic<int> finished{0};
    std::atomic<bool> stop{false};
    struct flag_receiver {
        std::atomic<bool> granted{false};
        void done(const stepwright::error&) noexcept {
            granted.store(true, std::memory_order_release);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            auto op = m.lock();
            flag_receiver receiver;
            int seen = 0;
            for (;;) {
                while (epoch.load(std::memory_order_acquire) == seen) {
                    if (stop.load(std::memory_order_acquire)) {
                        return;
                    }
                    std::this_thread::yield();
                }
                seen = epoch.load(std::memory_order_acquire);
                for (int i = 0; i < per_thread; ++i) {
                    receiver.granted.store(false, std::memory_order_relaxed);
                    op.start(stepwright::completion::bind<&flag_receiver::done>(&receiver));
                    while (!receiver.granted.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    m.unlock();
                }
                finished.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }
    for (auto _ : state) {
        finished.store(0, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_acq_rel);
        while (finished.load(std::memory_order_acquire) != threads) {
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& thread : pool) {
        thread.join();
    }
    state.SetItemsProcessed(state.iterations() * threads * per_thread);
}

void BM_shared_mutex_readers(benchmark::State& state) {
    const auto readers = static_cast<std::size_t>(state.range(0));
    stepwright::async_shared_mutex m;
    std::vector<std::unique_ptr<stepwright::async_shared_mutex::shared_lock_op>> ops;
    ops.reserve(readers);
    for (std::size_t i = 0; i < readers; ++i) {
        ops.push_back(std::make_unique<stepwright::async_shared_mutex::shared_lock_op>(m));
    }
    for (auto _ : state) {
        m.try_lock();
        for (auto& op : ops) {
            op->start(ignored); // queued behind the writer
        }
        m.unlock(); // admits every reader as one batch
        for (std::size_t i = 0; i < readers; ++i) {
            m.unlock_shared();
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

template <bool Any>
void run_sync_join(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<sync_child> children(n);
    std::vector<stepwright::task*> pointers;
    pointers.reserve(n);
    for (auto& child : children) {
        pointers.push_back(&child);
    }
    std::unique_ptr<stepwright::task> join;
    if constexpr (Any) {
        join = std::make_unique<stepwright::when_any_range>(pointers);
    } else {
        join = std::make_unique<stepwright::when_all_range>(pointers);
    }
    for (auto _ : state) {
        join->start(ignored);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
void BM_when_all_sync(benchmark::State& state) {
    run_sync_join<false>(state);
}
void BM_when_any_sync(benchmark::State& state) {
    run_sync_join<true>(state);
}

// Hand-written equivalent of when_all's bookkeeping for comparison: virtual
// start of each child plus one atomic decrement per completion, no tokens.
struct countdown {
    std::atomic<std::size_t> pending{0};
    stepwright::completion receiver;
    static void child_done(void* ctx, const stepwright::error&) noexcept {
        auto& self = *static_cast<countdown*>(ctx);
        if (self.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self.receiver({});
        }
    }
};
void BM_baseline_atomic_countdown(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<sync_child> children(n);
    std::vector<stepwright::task*> pointers;
    pointers.reserve(n);
    for (auto& child : children) {
        pointers.push_back(&child);
    }
    countdown join;
    join.receiver = ignored;
    for (auto _ : state) {
        join.pending.store(n + 1, std::memory_order_relaxed);
        for (auto* child : pointers) {
            child->start({.fn = &countdown::child_done, .ctx = &join});
        }
        countdown::child_done(&join, {});
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_when_all_async_manual(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    stepwright::manual_executor ex;
    std::vector<async_child> children(n);
    std::vector<stepwright::task*> pointers;
    pointers.reserve(n);
    for (auto& child : children) {
        child.ex = &ex;
        pointers.push_back(&child);
    }
    stepwright::when_all_range join(pointers);
    for (auto _ : state) {
        join.start(ignored);
        ex.run_all();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

struct noop_entry : stepwright::timer_service::entry {
    noop_entry() {
        fire = [](stepwright::timer_service::entry*) noexcept {};
    }
};
template <class Service>
void run_schedule_cancel(benchmark::State& state) {
    Service timers;
    noop_entry e;
    const auto deadline = timers.now() + std::chrono::hours(1);
    for (auto _ : state) {
        timers.schedule(e, deadline);
        bool removed = timers.cancel(e);
        benchmark::DoNotOptimize(removed);
    }
    state.SetItemsProcessed(state.iterations());
}
void BM_timer_schedule_cancel_manual(benchmark::State& state) {
    run_schedule_cancel<stepwright::testing::manual_timer_service>(state);
}
void BM_timer_schedule_cancel_steady(benchmark::State& state) {
    run_schedule_cancel<stepwright::steady_timer_service>(state);
}

void BM_with_deadline_inner_first(benchmark::State& state) {
    stepwright::testing::manual_timer_service timers;
    sync_child inner;
    stepwright::with_deadline guarded(inner, timers, std::chrono::seconds(1));
    for (auto _ : state) {
        guarded.start(ignored);
    }
    state.SetItemsProcessed(state.iterations());
}

// The timer wins: with_deadline cancels a pending delay (which completes with
// errc::cancelled) and completes with errc::timed_out.
void BM_deadline_timeout(benchmark::State& state) {
    stepwright::testing::manual_timer_service timers;
    stepwright::delay slow(timers, std::chrono::hours(1));
    stepwright::with_deadline guarded(slow, timers, std::chrono::milliseconds(1));
    for (auto _ : state) {
        guarded.start(ignored);
        timers.advance(std::chrono::milliseconds(1));
    }
    state.SetItemsProcessed(state.iterations());
}
} // namespace

BENCHMARK(BM_event_set_wait)->Arg(1)->Arg(1000);
BENCHMARK(BM_mutex_uncontended);
BENCHMARK(BM_baseline_std_mutex_uncontended);
BENCHMARK(BM_mutex_handoff_chain)->Arg(1000);
BENCHMARK(BM_mutex_contended_threads)->Arg(4)->UseRealTime();
BENCHMARK(BM_shared_mutex_readers)->Arg(1000);
BENCHMARK(BM_when_all_sync)->Arg(10)->Arg(1000);
BENCHMARK(BM_baseline_atomic_countdown)->Arg(10)->Arg(1000);
BENCHMARK(BM_when_all_async_manual)->Arg(1000);
BENCHMARK(BM_when_any_sync)->Arg(10);
BENCHMARK(BM_timer_schedule_cancel_manual);
BENCHMARK(BM_timer_schedule_cancel_steady);
BENCHMARK(BM_with_deadline_inner_first);
BENCHMARK(BM_deadline_timeout);
