#include <gtest/gtest.h>

#include "test_support.hpp"
#include <barrier>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using stepwright::errc;
using stepwright::error;

TEST(shared_task, lazy_start_on_first_wait_and_shared_result) {
    test_support::async_child inner;
    stepwright::shared_task shared(inner);
    EXPECT_FALSE(inner.receiver);
    auto first = shared.wait();
    auto second = shared.wait();
    test_support::receiver r1;
    test_support::receiver r2;
    first.start(r1.callback());
    EXPECT_TRUE(inner.receiver);
    second.start(r2.callback());
    EXPECT_FALSE(shared.is_done());
    inner.finish(error::make(errc::closed));
    EXPECT_TRUE(shared.is_done());
    for (auto* r : {&r1, &r2}) {
        EXPECT_EQ(r->calls, 1);
        EXPECT_TRUE(r->result.is(errc::closed));
    }
    auto late = shared.wait();
    test_support::receiver r3;
    late.start(r3.callback());
    EXPECT_EQ(r3.calls, 1);
    EXPECT_TRUE(r3.result.is(errc::closed));
}

TEST(shared_task, many_waiters_fifo) {
    constexpr int count = 1000;
    test_support::async_child inner;
    stepwright::shared_task shared(inner);
    std::vector<int> order;
    struct recorder {
        std::vector<int>* order;
        int index;
        void done(const error&) noexcept { order->push_back(index); }
    };
    std::vector<recorder> recorders;
    recorders.reserve(count);
    std::vector<std::unique_ptr<stepwright::shared_task::waiter>> waiters;
    for (int i = 0; i < count; ++i) {
        recorders.push_back({.order = &order, .index = i});
        waiters.push_back(std::make_unique<stepwright::shared_task::waiter>(shared));
        waiters.back()->start(stepwright::completion::bind<&recorder::done>(&recorders.back()));
    }
    inner.finish();
    ASSERT_EQ(order.size(), static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(shared_task, inner_completing_synchronously) {
    test_support::sync_child inner;
    stepwright::shared_task shared(inner);
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        auto waiter = shared.wait();
        test_support::receiver receiver;
        waiter.start(receiver.callback(), token);
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_TRUE(shared.is_done());
        shared.reset();
    }
    EXPECT_EQ(inner.calls, 2);
}

TEST(shared_task, waiter_cancellation_does_not_stop_inner) {
    test_support::cancellable_child inner;
    stepwright::shared_task shared(inner);
    stepwright::cancel_source source;
    auto waiter = shared.wait();
    test_support::receiver receiver;
    waiter.start(receiver.callback(), source.token());
    source.request_cancel();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_FALSE(inner.received_token.cancel_requested());
    EXPECT_FALSE(shared.is_done());
    // The inner task keeps running with no waiters; a new waiter joins it.
    auto again = shared.wait();
    test_support::receiver second;
    again.start(second.callback());
    EXPECT_EQ(inner.starts.load(), 1);
    inner.complete();
    EXPECT_EQ(second.calls, 1);
    EXPECT_FALSE(second.result);
}

TEST(shared_task, cancel_stops_inner_and_reset_allows_rerun) {
    test_support::cancellable_child inner;
    stepwright::shared_task shared(inner);
    shared.cancel(); // not running: no-op
    auto waiter = shared.wait();
    test_support::receiver receiver;
    waiter.start(receiver.callback());
    shared.cancel();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_TRUE(shared.is_done());
    shared.reset();
    test_support::receiver rerun;
    waiter.start(rerun.callback());
    EXPECT_FALSE(inner.received_token.cancel_requested());
    inner.complete();
    EXPECT_EQ(rerun.calls, 1);
    EXPECT_FALSE(rerun.result);
    EXPECT_EQ(inner.starts.load(), 2);
}

TEST(shared_task, concurrent_waiters_from_threads) {
    constexpr int threads = 8;
    constexpr int per_thread = 100;
    const int rounds = test_support::stress_iterations(50);
    struct flagged_child : stepwright::task {
        stepwright::completion receiver;
        std::atomic<bool> started{false};
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            receiver = done;
            started.store(true, std::memory_order_release);
        }
    } inner;
    stepwright::shared_task shared(inner);
    std::vector<std::unique_ptr<stepwright::shared_task::waiter>> waiters;
    waiters.reserve(static_cast<std::size_t>(threads) * per_thread);
    for (int i = 0; i < threads * per_thread; ++i) {
        waiters.push_back(std::make_unique<stepwright::shared_task::waiter>(shared));
    }
    std::atomic<int> completed{0};
    const auto bump =
        stepwright::completion{.fn =
                                   [](void* ctx, const error& e) noexcept {
                                       EXPECT_FALSE(e);
                                       static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
                                   },
                               .ctx = &completed};
    for (int round = 0; round < rounds; ++round) {
        completed = 0;
        std::barrier sync(threads + 1);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                sync.arrive_and_wait();
                for (int i = 0; i < per_thread; ++i) {
                    waiters[(static_cast<std::size_t>(t) * per_thread) +
                            static_cast<std::size_t>(i)]
                        ->start(bump);
                }
            });
        }
        sync.arrive_and_wait();
        // Finish the inner task while waiters are still arriving.
        while (!inner.started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        inner.started.store(false, std::memory_order_relaxed);
        const auto done = inner.receiver;
        done({});
        for (auto& worker : workers) {
            worker.join();
        }
        EXPECT_EQ(completed.load(), threads * per_thread);
        shared.reset();
    }
}

TEST(shared_task, regression_waiter_cancel_vs_inner_completion) {
    const int iterations = test_support::stress_iterations(20'000);
    test_support::async_child inner;
    stepwright::shared_task shared(inner);
    auto waiter = shared.wait();
    stepwright::cancel_source* source = nullptr;
    test_support::race_rounds rounds({[&] { source->request_cancel(); }});
    int cancelled = 0;
    unsigned seed = 1;
    for (int i = 0; i < iterations; ++i) {
        stepwright::cancel_source local;
        source = &local;
        test_support::receiver receiver;
        waiter.start(receiver.callback(), local.token());
        rounds.run_round([&] {
            test_support::jitter(seed, 20);
            inner.finish();
        });
        EXPECT_EQ(receiver.calls, 1);
        if (receiver.result) {
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
            ++cancelled;
        }
        shared.reset();
    }
    std::printf("cancelled %d of %d\n", cancelled, iterations);
}

TEST(shared_task, regression_destroy_running_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            test_support::async_child inner;
            auto shared = std::make_unique<stepwright::shared_task>(inner);
            stepwright::cancel_source source;
            auto waiter = shared->wait();
            test_support::receiver receiver;
            waiter.start(receiver.callback(), source.token());
            source.request_cancel();
            shared.reset();
        },
        "destroying a running shared_task");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
