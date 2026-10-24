#include <gtest/gtest.h>

#include "test_support.hpp"
#include <barrier>
#include <memory>
#include <thread>
#include <vector>

using stepwright::errc;
using stepwright::error;

TEST(when_any, single_child_wins) {
    test_support::sync_child a;
    a.result = error::make(errc::closed);
    stepwright::when_any any(a);
    test_support::receiver receiver;
    any.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::closed));
    EXPECT_EQ(any.winner(), 0U);
}

TEST(when_any, first_synchronous_child_wins_and_cancels_the_rest) {
    test_support::cancellable_child a;
    test_support::sync_child b;
    test_support::cancellable_child c;
    stepwright::when_any any(a, b, c);
    static_assert(std::is_same_v<decltype(any), stepwright::when_any<3>>);
    test_support::receiver receiver;
    any.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
    EXPECT_EQ(any.winner(), 1U);
    EXPECT_EQ(a.cancelled.load(), 1);
    EXPECT_EQ(c.cancelled.load(), 1); // started with an already-cancelled token
}

TEST(when_any, async_winner_and_structured_completion) {
    test_support::async_child a; // ignores cancellation
    test_support::async_child b;
    test_support::cancellable_child c;
    stepwright::when_any any(a, b, c);
    test_support::receiver receiver;
    any.start(receiver.callback());
    b.finish(error::make(errc::closed, "b"));
    EXPECT_EQ(c.cancelled.load(), 1);
    EXPECT_TRUE(a.received_token.cancel_requested());
    EXPECT_EQ(receiver.calls, 0); // a is still running
    a.finish();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_EQ(any.winner(), 1U);
    EXPECT_EQ(receiver.result.message(), "b");
}

TEST(when_any, parent_cancellation_and_restart) {
    test_support::cancellable_child a;
    test_support::cancellable_child b;
    stepwright::when_any any(a, b);
    for (int round = 0; round < 3; ++round) {
        stepwright::cancel_source source;
        test_support::receiver receiver;
        any.start(receiver.callback(), source.token());
        EXPECT_FALSE(a.received_token.cancel_requested());
        if (round == 1) {
            b.complete();
            EXPECT_EQ(any.winner(), 1U);
            EXPECT_FALSE(receiver.result);
        } else {
            source.request_cancel();
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
        }
        EXPECT_EQ(receiver.calls, 1);
    }
}

TEST(when_any, range_of_ten_thousand_children) {
    constexpr std::size_t count = 10'000;
    std::vector<test_support::cancellable_child> children(count);
    std::vector<stepwright::task*> pointers;
    pointers.reserve(children.size());
    for (auto& child : children) {
        pointers.push_back(&child);
    }
    stepwright::when_any_range any(pointers);
    for (std::size_t round = 0; round < 2; ++round) {
        test_support::receiver receiver;
        any.start(receiver.callback());
        const std::size_t pick = round == 0 ? 4321 : count - 1;
        children[pick].complete(error::make(errc::timed_out));
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_EQ(any.winner(), pick);
        EXPECT_TRUE(receiver.result.is(errc::timed_out));
    }
}

TEST(when_any, regression_children_race_on_many_threads) {
    constexpr std::size_t count = 16;
    const int rounds = test_support::stress_iterations(300);
    std::vector<test_support::async_child> children(count);
    std::vector<stepwright::task*> pointers;
    pointers.reserve(children.size());
    for (auto& child : children) {
        pointers.push_back(&child);
    }
    stepwright::when_any_range any(pointers);
    std::vector<std::size_t> wins(count, 0);
    for (int round = 0; round < rounds; ++round) {
        test_support::receiver receiver;
        any.start(receiver.callback());
        std::barrier sync(static_cast<std::ptrdiff_t>(count));
        std::vector<std::thread> threads;
        threads.reserve(count);
        for (std::size_t t = 0; t < count; ++t) {
            threads.emplace_back([&, t] {
                sync.arrive_and_wait();
                children[t].finish(t % 2 == 0 ? error{} : error::make(errc::closed));
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
        ASSERT_LT(any.winner(), count);
        EXPECT_EQ(receiver.result.is(errc::closed), any.winner() % 2 == 1);
        ++wins[any.winner()];
    }
}

TEST(when_any, regression_zero_children_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::when_any_range any(std::span<stepwright::task* const>{});
            test_support::receiver receiver;
            any.start(receiver.callback());
        },
        "when_any requires at least one child");
#else
    stepwright::when_any_range any(std::span<stepwright::task* const>{});
    test_support::receiver receiver;
    any.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_EQ(any.winner(), stepwright::when_any_range::npos);
#endif
}
