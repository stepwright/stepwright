#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
using stepwright::errc;
using stepwright::error;
using stepwright::testing::manual_timer_service;

TEST(delay, completes_after_duration_with_manual_timers) {
    manual_timer_service timers;
    stepwright::delay sleep(timers, 10ms);
    for (int round = 0; round < 3; ++round) {
        test_support::receiver receiver;
        sleep.start(receiver.callback());
        EXPECT_EQ(timers.pending(), 1U);
        timers.advance(9ms);
        EXPECT_EQ(receiver.calls, 0);
        timers.advance(1ms);
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_FALSE(receiver.result);
    }
}

TEST(delay, cancellation_cancels_the_timer) {
    manual_timer_service timers;
    stepwright::delay sleep(timers, 10ms);
    stepwright::cancel_source source;
    test_support::receiver receiver;
    sleep.start(receiver.callback(), source.token());
    source.request_cancel();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_EQ(timers.pending(), 0U);
    timers.advance(1s);
    EXPECT_EQ(receiver.calls, 1);
    stepwright::cancel_source pre;
    pre.request_cancel();
    test_support::receiver immediate;
    sleep.start(immediate.callback(), pre.token());
    EXPECT_TRUE(immediate.result.is(errc::cancelled));
    EXPECT_EQ(timers.pending(), 0U);
    // Cancellation after firing is a no-op.
    stepwright::cancel_source late;
    test_support::receiver fired;
    sleep.start(fired.callback(), late.token());
    timers.advance(10ms);
    late.request_cancel();
    EXPECT_EQ(fired.calls, 1);
    EXPECT_FALSE(fired.result);
}

TEST(delay, steady_service_fires) {
    stepwright::steady_timer_service timers;
    stepwright::delay sleep(timers, 2ms);
    test_support::receiver receiver;
    const auto start = std::chrono::steady_clock::now();
    sleep.start(receiver.callback());
    receiver.wait();
    EXPECT_GE(std::chrono::steady_clock::now() - start, 2ms);
    EXPECT_FALSE(receiver.result);
}

TEST(delay, regression_cancel_vs_fire_race_steady) {
    stepwright::steady_timer_service timers;
    const int iterations = test_support::stress_iterations(2000);
    stepwright::cancel_source* source = nullptr;
    unsigned seed = 7;
    test_support::race_rounds rounds({[&] {
        test_support::jitter(seed, 80);
        source->request_cancel();
    }});
    int cancelled = 0;
    stepwright::delay sleep(timers, 40us);
    for (int i = 0; i < iterations; ++i) {
        stepwright::cancel_source local;
        source = &local;
        test_support::receiver receiver;
        sleep.start(receiver.callback(), local.token());
        rounds.run_round();
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
        if (receiver.result) {
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
            ++cancelled;
        }
    }
    std::printf("cancelled %d of %d\n", cancelled, iterations);
}

TEST(with_deadline, inner_first_cancels_timer) {
    manual_timer_service timers;
    test_support::async_child inner;
    stepwright::with_deadline guarded(inner, timers, 50ms);
    for (int round = 0; round < 2; ++round) {
        test_support::receiver receiver;
        guarded.start(receiver.callback());
        EXPECT_EQ(timers.pending(), 1U);
        timers.advance(10ms);
        inner.finish(round == 0 ? error{} : error::make(errc::closed));
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_EQ(receiver.result.is(errc::closed), round == 1);
        EXPECT_EQ(timers.pending(), 0U);
        EXPECT_FALSE(inner.received_token.cancel_requested());
    }
}

TEST(with_deadline, timer_first_cancels_inner_and_times_out) {
    manual_timer_service timers;
    test_support::cancellable_child inner;
    stepwright::with_deadline guarded(inner, timers, 50ms);
    test_support::receiver receiver;
    guarded.start(receiver.callback());
    timers.advance(50ms);
    EXPECT_EQ(inner.cancelled.load(), 1);
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::timed_out));
    // Restart after a timed-out run: the internal token is fresh again.
    test_support::receiver second;
    guarded.start(second.callback());
    EXPECT_FALSE(inner.received_token.cancel_requested());
    inner.complete();
    EXPECT_FALSE(second.result);
    EXPECT_EQ(timers.pending(), 0U);
}

TEST(with_deadline, structured_completion_when_inner_ignores_cancellation) {
    manual_timer_service timers;
    test_support::async_child inner; // ignores cancellation
    stepwright::with_deadline guarded(inner, timers, 5ms);
    test_support::receiver receiver;
    guarded.start(receiver.callback());
    timers.advance(1s);
    EXPECT_TRUE(inner.received_token.cancel_requested());
    EXPECT_EQ(receiver.calls, 0);
    inner.finish(); // even a late success is reported as timed_out
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::timed_out));
}

TEST(with_deadline, parent_cancellation) {
    manual_timer_service timers;
    test_support::cancellable_child inner;
    stepwright::with_deadline guarded(inner, timers, 50ms);
    stepwright::cancel_source source;
    test_support::receiver receiver;
    guarded.start(receiver.callback(), source.token());
    source.request_cancel();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_EQ(inner.cancelled.load(), 1);
    EXPECT_EQ(timers.pending(), 0U);
    // Parent already cancelled at start.
    test_support::receiver pre;
    guarded.start(pre.callback(), source.token());
    EXPECT_TRUE(pre.result.is(errc::cancelled));
    EXPECT_EQ(timers.pending(), 0U);
    // Parent cancellation with an inner that ignores it waits for the inner.
    test_support::async_child stubborn;
    stepwright::with_deadline waiting(stubborn, timers, 50ms);
    stepwright::cancel_source other;
    test_support::receiver r;
    waiting.start(r.callback(), other.token());
    other.request_cancel();
    EXPECT_EQ(r.calls, 0);
    EXPECT_EQ(timers.pending(), 0U);
    stubborn.finish();
    EXPECT_TRUE(r.result.is(errc::cancelled));
}

TEST(with_deadline, regression_inner_vs_timer_race_steady) {
    stepwright::steady_timer_service timers;
    const int iterations = test_support::stress_iterations(2000);
    test_support::cancellable_child inner;
    stepwright::with_deadline guarded(inner, timers, 30us);
    unsigned seed = 11;
    test_support::race_rounds rounds({[&] {
        test_support::jitter(seed, 60);
        inner.complete();
    }});
    int timed_out = 0;
    for (int i = 0; i < iterations; ++i) {
        test_support::receiver receiver;
        guarded.start(receiver.callback());
        rounds.run_round();
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
        if (receiver.result) {
            EXPECT_TRUE(receiver.result.is(errc::timed_out));
            ++timed_out;
        }
    }
    std::printf("timed out %d of %d\n", timed_out, iterations);
}

TEST(with_deadline, regression_three_way_race_steady) {
    stepwright::steady_timer_service timers;
    const int iterations = test_support::stress_iterations(2000);
    test_support::cancellable_child inner;
    stepwright::with_deadline guarded(inner, timers, 20us);
    stepwright::cancel_source* source = nullptr;
    unsigned complete_seed = 3;
    unsigned cancel_seed = 5;
    test_support::race_rounds rounds({[&] {
                                          test_support::jitter(complete_seed, 40);
                                          inner.complete();
                                      },
                                      [&] {
                                          test_support::jitter(cancel_seed, 40);
                                          source->request_cancel();
                                      }});
    std::array<int, 3> outcomes{};
    for (int i = 0; i < iterations; ++i) {
        stepwright::cancel_source local;
        source = &local;
        test_support::receiver receiver;
        guarded.start(receiver.callback(), local.token());
        rounds.run_round();
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
        if (!receiver.result) {
            ++outcomes[0];
        } else if (receiver.result.is(errc::timed_out)) {
            ++outcomes[1];
        } else {
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
            ++outcomes[2];
        }
    }
    std::printf("inner %d, timed out %d, cancelled %d\n", outcomes[0], outcomes[1], outcomes[2]);
}
