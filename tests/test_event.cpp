#include <gtest/gtest.h>

#include "test_support.hpp"
#include <barrier>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using stepwright::errc;
using stepwright::error;

namespace {
struct order_receiver {
    std::vector<int>* order = nullptr;
    int index = 0;
    error result;
    int calls = 0;
    void finish(const error& e) noexcept {
        result = e;
        ++calls;
        order->push_back(index);
    }
};
} // namespace

TEST(event, wait_after_set_completes_synchronously) {
    stepwright::event ev;
    EXPECT_FALSE(ev.is_set());
    ev.set();
    EXPECT_TRUE(ev.is_set());
    auto waiter = ev.wait();
    test_support::receiver receiver;
    waiter.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
}

TEST(event, wait_before_set_completes_on_set) {
    stepwright::event ev;
    auto waiter = ev.wait();
    test_support::receiver receiver;
    waiter.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    ev.set();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
}

TEST(event, set_error_is_propagated_to_all_waiters) {
    stepwright::event ev;
    auto early = ev.wait();
    test_support::receiver early_receiver;
    early.start(early_receiver.callback());
    ev.set(error::make(errc::closed, "gone"));
    auto late = ev.wait();
    test_support::receiver late_receiver;
    late.start(late_receiver.callback());
    for (auto* r : {&early_receiver, &late_receiver}) {
        EXPECT_EQ(r->calls, 1);
        EXPECT_TRUE(r->result.is(errc::closed));
        EXPECT_EQ(r->result.message(), "gone");
    }
}

TEST(event, many_waiters_complete_in_fifo_order) {
    constexpr int count = 10'000;
    stepwright::event ev;
    std::vector<int> order;
    order.reserve(count);
    std::vector<std::unique_ptr<stepwright::event::waiter>> waiters;
    std::vector<order_receiver> receivers(count);
    for (int i = 0; i < count; ++i) {
        waiters.push_back(std::make_unique<stepwright::event::waiter>(ev));
        receivers[static_cast<std::size_t>(i)].order = &order;
        receivers[static_cast<std::size_t>(i)].index = i;
        waiters.back()->start(stepwright::completion::bind<&order_receiver::finish>(
            &receivers[static_cast<std::size_t>(i)]));
    }
    EXPECT_TRUE(order.empty());
    ev.set();
    ASSERT_EQ(order.size(), static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(event, cancellation_while_waiting_unregisters) {
    stepwright::event ev;
    stepwright::cancel_source source;
    auto first = ev.wait();
    auto second = ev.wait();
    auto third = ev.wait();
    test_support::receiver r1;
    test_support::receiver r2;
    test_support::receiver r3;
    first.start(r1.callback());
    second.start(r2.callback(), source.token());
    third.start(r3.callback());
    source.request_cancel();
    EXPECT_EQ(r2.calls, 1);
    EXPECT_TRUE(r2.result.is(errc::cancelled));
    EXPECT_EQ(r1.calls + r3.calls, 0);
    ev.set();
    EXPECT_EQ(r1.calls, 1);
    EXPECT_EQ(r3.calls, 1);
    EXPECT_EQ(r2.calls, 1);
    EXPECT_FALSE(r1.result);
}

TEST(event, pre_cancelled_token_while_unset_cancels_and_while_set_completes) {
    stepwright::event ev;
    stepwright::cancel_source source;
    source.request_cancel();
    auto waiter = ev.wait();
    test_support::receiver cancelled;
    waiter.start(cancelled.callback(), source.token());
    EXPECT_EQ(cancelled.calls, 1);
    EXPECT_TRUE(cancelled.result.is(errc::cancelled));
    ev.set();
    test_support::receiver completed;
    waiter.start(completed.callback(), source.token());
    EXPECT_EQ(completed.calls, 1);
    EXPECT_FALSE(completed.result);
}

TEST(event, cancel_after_completion_is_noop) {
    stepwright::event ev;
    stepwright::cancel_source source;
    auto waiter = ev.wait();
    test_support::receiver receiver;
    waiter.start(receiver.callback(), source.token());
    ev.set();
    source.request_cancel();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
}

TEST(event, restart_and_reset) {
    stepwright::event ev;
    auto waiter = ev.wait();
    for (int round = 0; round < 3; ++round) {
        test_support::receiver receiver;
        waiter.start(receiver.callback());
        EXPECT_EQ(receiver.calls, 0);
        ev.set(round == 1 ? error::make(errc::timed_out) : error{});
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_EQ(receiver.result.is(errc::timed_out), round == 1);
        ev.reset();
        EXPECT_FALSE(ev.is_set());
    }
}

TEST(event, completion_may_destroy_waiter_and_restart_others) {
    stepwright::event ev;
    auto owned = std::make_unique<stepwright::event::waiter>(ev);
    owned->start({.fn =
                      [](void* ctx, const error&) noexcept {
                          static_cast<std::unique_ptr<stepwright::event::waiter>*>(ctx)->reset();
                      },
                  .ctx = &owned});
    ev.set();
    EXPECT_FALSE(owned);
}

TEST(event, regression_cancel_vs_set_race) {
    const int iterations = test_support::stress_iterations(20'000);
    stepwright::event* ev = nullptr;
    stepwright::cancel_source* source = nullptr;
    test_support::race_rounds rounds({[&] { ev->set(); }, [&] { source->request_cancel(); }});
    int cancelled = 0;
    int succeeded = 0;
    for (int i = 0; i < iterations; ++i) {
        stepwright::event local_event;
        stepwright::cancel_source local_source;
        auto waiter = local_event.wait();
        test_support::receiver receiver;
        waiter.start(receiver.callback(), local_source.token());
        ev = &local_event;
        source = &local_source;
        rounds.run_round();
        EXPECT_EQ(receiver.calls, 1);
        if (receiver.result.is(errc::cancelled)) {
            ++cancelled;
        } else {
            EXPECT_FALSE(receiver.result);
            ++succeeded;
        }
    }
    EXPECT_EQ(cancelled + succeeded, iterations);
    std::printf("cancelled %d, set %d\n", cancelled, succeeded);
}

TEST(event, regression_start_vs_set_race_with_token) {
    // The waiter publishes itself before arming cancellation; a concurrent set()
    // must be completed by whichever side finishes last, exactly once.
    const int iterations = test_support::stress_iterations(20'000);
    stepwright::event* ev = nullptr;
    test_support::race_rounds rounds({[&] { ev->set(); }});
    stepwright::cancel_source source;
    for (int i = 0; i < iterations; ++i) {
        stepwright::event local_event;
        auto waiter = local_event.wait();
        test_support::receiver receiver;
        ev = &local_event;
        rounds.run_round([&] { waiter.start(receiver.callback(), source.token()); });
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_FALSE(receiver.result);
    }
}

TEST(event, concurrent_waiters_from_threads) {
    constexpr int threads = 8;
    constexpr int per_thread = 200;
    stepwright::event ev;
    std::atomic<int> completed{0};
    std::vector<std::unique_ptr<stepwright::event::waiter>> waiters;
    waiters.reserve(static_cast<std::size_t>(threads) * per_thread);
    for (int i = 0; i < threads * per_thread; ++i) {
        waiters.push_back(std::make_unique<stepwright::event::waiter>(ev));
    }
    auto bump = stepwright::completion{
        .fn = [](void* ctx,
                 const error&) noexcept { static_cast<std::atomic<int>*>(ctx)->fetch_add(1); },
        .ctx = &completed};
    std::barrier sync(threads + 1);
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < per_thread; ++i) {
                waiters[(static_cast<std::size_t>(t) * per_thread) + static_cast<std::size_t>(i)]
                    ->start(bump);
            }
        });
    }
    sync.arrive_and_wait();
    ev.set();
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(completed.load(), threads * per_thread);
}

TEST(event, regression_destroy_with_waiters_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            auto ev = std::make_unique<stepwright::event>();
            auto waiter = std::make_unique<stepwright::event::waiter>(*ev);
            test_support::receiver receiver;
            waiter->start(receiver.callback());
            ev.reset();
        },
        "destroying an event with registered waiters");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}

TEST(event, regression_destroy_started_waiter_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::event ev;
            auto waiter = std::make_unique<stepwright::event::waiter>(ev);
            test_support::receiver receiver;
            waiter->start(receiver.callback());
            waiter.reset();
        },
        "destroying a started stepwright waiter");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}

TEST(event, second_set_is_ignored) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::event ev;
            ev.set();
            ev.set();
        },
        "event set twice");
#else
    stepwright::event ev;
    ev.set(error::make(errc::closed));
    ev.set();
    auto waiter = ev.wait();
    test_support::receiver receiver;
    waiter.start(receiver.callback());
    EXPECT_TRUE(receiver.result.is(errc::closed));
#endif
}
