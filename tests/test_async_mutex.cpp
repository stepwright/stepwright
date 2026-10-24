#include <gtest/gtest.h>

#include "test_support.hpp"
#include <barrier>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using stepwright::errc;
using stepwright::error;

TEST(async_mutex, uncontended_lock_is_synchronous) {
    stepwright::async_mutex m;
    auto op = m.lock();
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(m.try_lock());
    m.unlock();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_mutex, fifo_handoff_without_barging) {
    stepwright::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    std::vector<int> order;
    struct recorder {
        std::vector<int>* order;
        int index;
        void done(const error&) noexcept { order->push_back(index); }
    };
    std::vector<recorder> recorders{{.order = &order, .index = 0},
                                    {.order = &order, .index = 1},
                                    {.order = &order, .index = 2}};
    std::vector<std::unique_ptr<stepwright::async_mutex::lock_op>> ops;
    for (auto& r : recorders) {
        ops.push_back(std::make_unique<stepwright::async_mutex::lock_op>(m));
        ops.back()->start(stepwright::completion::bind<&recorder::done>(&r));
    }
    EXPECT_TRUE(order.empty());
    for (int i = 0; i < 3; ++i) {
        // Ownership transfers directly: never free while waiters are queued.
        EXPECT_FALSE(m.try_lock());
        m.unlock();
        ASSERT_EQ(order.size(), static_cast<std::size_t>(i + 1));
        EXPECT_EQ(order.back(), i);
    }
    EXPECT_FALSE(m.try_lock()); // held by the last waiter
    m.unlock();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_mutex, cancelled_waiter_unregisters_without_acquiring) {
    stepwright::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    stepwright::cancel_source source;
    auto first = m.lock();
    auto second = m.lock();
    auto third = m.lock();
    test_support::receiver r1;
    test_support::receiver r2;
    test_support::receiver r3;
    first.start(r1.callback());
    second.start(r2.callback(), source.token());
    third.start(r3.callback());
    source.request_cancel();
    EXPECT_EQ(r2.calls, 1);
    EXPECT_TRUE(r2.result.is(errc::cancelled));
    m.unlock();
    EXPECT_EQ(r1.calls, 1);
    EXPECT_EQ(r3.calls, 0);
    m.unlock();
    EXPECT_EQ(r3.calls, 1);
    m.unlock();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_mutex, pre_cancelled_waiter_when_contended) {
    stepwright::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    stepwright::cancel_source source;
    source.request_cancel();
    auto op = m.lock();
    test_support::receiver receiver;
    op.start(receiver.callback(), source.token());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    m.unlock();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_mutex, regression_cancel_vs_grant_race) {
    const int iterations = test_support::stress_iterations(20'000);
    stepwright::async_mutex m;
    stepwright::cancel_source* source = nullptr;
    test_support::race_rounds rounds({[&] { m.unlock(); }, [&] { source->request_cancel(); }});
    auto op = m.lock();
    int granted = 0;
    for (int i = 0; i < iterations; ++i) {
        ASSERT_TRUE(m.try_lock());
        stepwright::cancel_source local;
        source = &local;
        test_support::receiver receiver;
        op.start(receiver.callback(), local.token());
        rounds.run_round();
        EXPECT_EQ(receiver.calls, 1);
        if (!receiver.result) {
            ++granted;
            EXPECT_FALSE(m.try_lock()); // the waiter owns it
            m.unlock();
        } else {
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
        }
        ASSERT_TRUE(m.try_lock()); // never leaked
        m.unlock();
    }
    std::printf("granted %d of %d\n", granted, iterations);
}

TEST(async_mutex, guarded_runs_inner_under_lock_and_unlocks_on_failure) {
    stepwright::async_mutex m;
    struct checker : stepwright::task {
        stepwright::async_mutex* m = nullptr;
        error result;
        int calls = 0;
        bool was_locked = false;
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            ++calls;
            was_locked = !m->try_lock();
            if (!was_locked) {
                m->unlock();
            }
            const auto copy = result;
            done(copy);
        }
    } inner;
    inner.m = &m;
    inner.result = error::make(errc::closed);
    stepwright::async_mutex::guarded op(m, inner);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::closed));
    EXPECT_TRUE(inner.was_locked);
    EXPECT_TRUE(m.try_lock());
    m.unlock();
    inner.result = {};
    test_support::receiver again;
    op.start(again.callback());
    EXPECT_FALSE(again.result);
    EXPECT_EQ(inner.calls, 2);
}

TEST(async_mutex, guarded_forwards_token_and_cancelled_lock_skips_inner) {
    stepwright::async_mutex m;
    test_support::cancellable_child inner;
    stepwright::async_mutex::guarded op(m, inner);
    stepwright::cancel_source source;
    test_support::receiver receiver;
    op.start(receiver.callback(), source.token());
    EXPECT_EQ(inner.starts.load(), 1);
    source.request_cancel(); // reaches the inner task through the guard
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_TRUE(m.try_lock()); // unlocked after the failing inner
    stepwright::cancel_source second;
    test_support::receiver queued;
    op.start(queued.callback(), second.token());
    second.request_cancel(); // cancels the lock wait
    EXPECT_EQ(queued.calls, 1);
    EXPECT_TRUE(queued.result.is(errc::cancelled));
    EXPECT_EQ(inner.starts.load(), 1);
    m.unlock();
}

namespace {
struct chain_locker : stepwright::machine<chain_locker> {
    explicit chain_locker(stepwright::async_mutex& m) : mutex(&m), op(m) {}
    stepwright::async_mutex* mutex;
    stepwright::async_mutex::lock_op op;
    int* remaining = nullptr;
    test_support::depth_probe* probe = nullptr;
    step begin() { return after<&chain_locker::locked>(op); }
    step locked() {
        probe->sample();
        const bool more = (*remaining)-- > 0;
        mutex->unlock();
        return more ? then<&chain_locker::begin>() : done();
    }
};
} // namespace

TEST(async_mutex, regression_stack_handoff_chain_million) {
    stepwright::async_mutex m;
    test_support::depth_probe probe;
    int remaining = test_support::stress_iterations(1'000'000);
    std::vector<std::unique_ptr<chain_locker>> lockers;
    std::vector<test_support::receiver> receivers(3);
    ASSERT_TRUE(m.try_lock());
    for (auto& receiver : receivers) {
        lockers.push_back(std::make_unique<chain_locker>(m));
        lockers.back()->remaining = &remaining;
        lockers.back()->probe = &probe;
        lockers.back()->start(receiver.callback());
    }
    m.unlock();
    for (auto& receiver : receivers) {
        EXPECT_EQ(receiver.calls, 1);
    }
    EXPECT_LT(remaining, 0);
    EXPECT_LT(probe.span(), 64U * 1024U);
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_mutex, regression_contention_exclusivity_threads) {
    constexpr int threads = 8;
    constexpr int machines_per_thread = 4;
    const int iterations = test_support::stress_iterations(500);
    stepwright::async_mutex m;
    std::atomic<int> inside{0};
    std::atomic<int> violations{0};
    long long shared_counter = 0; // plain: TSan flags any overlap
    struct critical : stepwright::task {
        std::atomic<int>* inside = nullptr;
        std::atomic<int>* violations = nullptr;
        long long* counter = nullptr;
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            if (inside->fetch_add(1) != 0) {
                violations->fetch_add(1);
            }
            if (++*counter % 16 == 0) {
                std::this_thread::yield(); // widen the window for overlap
            }
            inside->fetch_sub(1);
            done({});
        }
    };
    struct worker : stepwright::machine<worker> {
        worker(stepwright::async_mutex& m, critical& cs) : guard(m, cs) {}
        stepwright::async_mutex::guarded guard;
        int remaining = 0;
        step begin() {
            if (remaining-- == 0) {
                return done();
            }
            return after<&worker::begin>(guard);
        }
    };
    critical cs;
    cs.inside = &inside;
    cs.violations = &violations;
    cs.counter = &shared_counter;
    std::vector<std::unique_ptr<worker>> workers;
    for (int i = 0; i < threads * machines_per_thread; ++i) {
        workers.push_back(std::make_unique<worker>(m, cs));
        workers.back()->remaining = iterations;
    }
    std::atomic<int> finished{0};
    const auto count_done = stepwright::completion{
        .fn =
            [](void* ctx, const error& e) noexcept {
                EXPECT_FALSE(e);
                static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_release);
            },
        .ctx = &finished};
    std::barrier sync(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < machines_per_thread; ++i) {
                workers[(static_cast<std::size_t>(t) * machines_per_thread) +
                        static_cast<std::size_t>(i)]
                    ->start(count_done);
            }
        });
    }
    for (auto& thread : pool) {
        thread.join();
    }
    while (finished.load(std::memory_order_acquire) != threads * machines_per_thread) {
        std::this_thread::yield();
    }
    EXPECT_EQ(violations.load(), 0);
    EXPECT_EQ(shared_counter, static_cast<long long>(threads) * machines_per_thread * iterations);
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_mutex, regression_destroy_locked_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::async_mutex m;
            m.try_lock();
        },
        "destroying a locked async_mutex");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
