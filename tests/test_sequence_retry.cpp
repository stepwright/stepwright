#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <deque>
#include <limits>
#include <memory>

using namespace std::chrono_literals;
namespace {
struct ordered_child : stepwright::task {
    std::vector<int>* order = nullptr;
    int index = 0;
    stepwright::error result;
    void on_start(stepwright::completion done, stepwright::cancel_token) override {
        order->push_back(index);
        const auto copy = result;
        done(copy);
    }
};
struct flaky : stepwright::task {
    explicit flaky(stepwright::timer_service& timers) : clock(timers) {}
    stepwright::timer_service& clock;
    int failures = 2;
    int calls = 0;
    std::array<stepwright::timer_service::clock::duration, 8> times{};
    stepwright::error failure = stepwright::error::make(std::errc::io_error, "flaky");
    void on_start(stepwright::completion done, stepwright::cancel_token) override {
        if (std::cmp_less(calls, times.size())) {
            times[static_cast<std::size_t>(calls)] = clock.now().time_since_epoch();
        }
        ++calls;
        const auto result = calls <= failures ? failure : stepwright::error{};
        done(result);
    }
};
} // namespace

TEST(sequence, order_failure_short_circuit_and_restart) {
    std::vector<int> order;
    std::array<ordered_child, 3> children;
    for (std::size_t i = 0; i < children.size(); ++i) {
        children[i].order = &order;
        children[i].index = static_cast<int>(i);
    }
    stepwright::sequence fixed(children[0], children[1], children[2]);
    EXPECT_FALSE(stepwright::sync_wait(fixed));
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
    order.clear();
    children[1].result = stepwright::error::make(stepwright::errc::closed, "middle");
    EXPECT_EQ(stepwright::sync_wait(fixed).message(), "middle");
    EXPECT_EQ(order, (std::vector<int>{0, 1}));
    order.clear();
    children[1].result = {};
    EXPECT_FALSE(stepwright::sync_wait(fixed));
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
    stepwright::sequence empty;
    stepwright::sequence_range empty_range(std::span<stepwright::task* const>{});
    stepwright::cancel_source stop;
    stop.request_cancel();
    EXPECT_FALSE(stepwright::sync_wait(empty, stop.token()));
    EXPECT_FALSE(stepwright::sync_wait(empty_range, stop.token()));
}

TEST(sequence, token_forwarding_async_children_and_cancellation_between_them) {
    stepwright::cancel_source stop;
    auto first = stepwright::make_task(
        [&](stepwright::completion done, const stepwright::cancel_token& token) {
            EXPECT_TRUE(token.can_be_cancelled());
            stop.request_cancel();
            done({});
        });
    test_support::sync_child second;
    stepwright::sequence cancelled(first, second);
    EXPECT_TRUE(stepwright::sync_wait(cancelled, stop.token()).is(stepwright::errc::cancelled));
    EXPECT_EQ(second.calls, 0);
    stepwright::testing::virtual_executor ex;
    stepwright::reschedule hop(ex);
    stepwright::sequence async(hop, second);
    EXPECT_FALSE(ex.run(async));
    EXPECT_EQ(second.calls, 1);
    test_support::cancellable_child child;
    stepwright::sequence waiting(child, second);
    test_support::receiver receiver;
    stepwright::cancel_source cancel;
    waiting.start(receiver.callback(), cancel.token());
    cancel.request_cancel();
    EXPECT_TRUE(receiver.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(second.calls, 1);
}

TEST(sequence, hundred_thousand_synchronous_children_have_constant_stack) {
    struct probed : stepwright::task {
        test_support::depth_probe* depth = nullptr;
        std::size_t* calls = nullptr;
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            depth->sample();
            ++*calls;
            done({});
        }
    };
    std::vector<probed> children(100'000);
    test_support::depth_probe depth;
    std::size_t calls = 0;
    for (auto& child : children) {
        child.depth = &depth;
        child.calls = &calls;
    }
    stepwright::sequence_range sequence(children);
    EXPECT_FALSE(stepwright::sync_wait(sequence));
    EXPECT_EQ(calls, children.size());
    EXPECT_LT(depth.span(), 16'384U);
}

TEST(sequence, completion_may_destroy_or_restart_and_foreign_payload_is_visible) {
    test_support::sync_child first;
    test_support::sync_child second;
    auto op = std::make_unique<stepwright::sequence<2>>(first, second);
    auto destroy = [&](const stepwright::error& e) {
        EXPECT_FALSE(e);
        op.reset();
    };
    op->start(stepwright::completion::to(destroy));
    EXPECT_EQ(first.calls, 1);
    stepwright::sequence again(first, second);
    test_support::receiver final;
    auto restart = [&](const stepwright::error& e) {
        EXPECT_FALSE(e);
        again.start(final.callback());
    };
    again.start(stepwright::completion::to(restart));
    EXPECT_EQ(final.calls, 1);
    stepwright::thread_pool pool(2);
    stepwright::reschedule hop(pool);
    int payload = 0;
    auto write =
        stepwright::make_task([&](stepwright::completion done, const stepwright::cancel_token&) {
            payload = 42;
            done({});
        });
    stepwright::sequence foreign(hop, write);
    EXPECT_FALSE(stepwright::sync_wait(foreign));
    EXPECT_EQ(payload, 42);
    pool.wait_idle();
}

TEST(retry, exact_exponential_timestamps_cap_and_restart) {
    stepwright::testing::virtual_executor ex;
    flaky child(ex);
    child.failures = 4;
    stepwright::retry retry(
        child, ex, {.max_attempts = 5, .initial_backoff = 2s, .multiplier = 2, .max_backoff = 5s});
    EXPECT_FALSE(ex.run(retry));
    EXPECT_EQ(retry.attempts(), 5U);
    EXPECT_EQ(child.times[0], 0s);
    EXPECT_EQ(child.times[1], 2s);
    EXPECT_EQ(child.times[2], 6s);
    EXPECT_EQ(child.times[3], 11s);
    EXPECT_EQ(child.times[4], 16s);
    child.calls = 0;
    child.failures = 1;
    EXPECT_FALSE(ex.run(retry));
    EXPECT_EQ(retry.attempts(), 2U);
    EXPECT_EQ(child.times[1] - child.times[0], 2s);
}

TEST(retry, exhaustion_nonretryable_default_and_custom_predicate) {
    stepwright::testing::virtual_executor ex;
    flaky child(ex);
    child.failures = 100;
    stepwright::retry once(child, ex, {.max_attempts = 1});
    EXPECT_EQ(ex.run(once).message(), "flaky");
    EXPECT_EQ(once.attempts(), 1U);
    stepwright::retry never(
        child, ex, {.should_retry = [](const stepwright::error&) noexcept { return false; }});
    EXPECT_EQ(ex.run(never).message(), "flaky");
    EXPECT_EQ(never.attempts(), 1U);
    child.failure = stepwright::error::make(stepwright::errc::cancelled);
    stepwright::retry default_filter(child, ex);
    EXPECT_TRUE(ex.run(default_filter).is(stepwright::errc::cancelled));
    EXPECT_EQ(default_filter.attempts(), 1U);
}

TEST(retry, cancellation_during_backoff_and_before_first_attempt) {
    stepwright::testing::virtual_executor ex;
    flaky child(ex);
    stepwright::retry retry(child, ex, {.initial_backoff = 10s});
    stepwright::cancel_source stop;
    test_support::receiver receiver;
    retry.start(receiver.callback(), stop.token());
    ex.run_for(3s);
    EXPECT_EQ(retry.attempts(), 1U);
    stop.request_cancel();
    EXPECT_TRUE(receiver.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(ex.scheduled_timers(), 0U);
    EXPECT_TRUE(ex.run(retry, stop.token()).is(stepwright::errc::cancelled));
    EXPECT_EQ(retry.attempts(), 0U);
    child.calls = 0;
    child.failures = 0;
    EXPECT_FALSE(ex.run(retry));
    EXPECT_EQ(retry.attempts(), 1U);
}

TEST(retry, waits_for_cancellation_resistant_inner_and_supports_receiver_teardown) {
    stepwright::testing::virtual_executor ex;
    test_support::async_child child;
    stepwright::retry retry(child, ex);
    stepwright::cancel_source stop;
    test_support::receiver receiver;
    retry.start(receiver.callback(), stop.token());
    stop.request_cancel();
    EXPECT_EQ(receiver.calls, 0);
    child.finish(); // inner success wins; cancellation is cooperative
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
    test_support::sync_child sync;
    auto owned = std::make_unique<stepwright::retry>(sync, ex);
    auto destroy = [&](const stepwright::error& e) {
        EXPECT_FALSE(e);
        owned.reset();
    };
    owned->start(stepwright::completion::to(destroy));
    EXPECT_EQ(sync.calls, 1);
}

TEST(retry, zero_backoff_large_multiplier_initial_cap_and_fractional_ticks) {
    stepwright::testing::virtual_executor ex;
    flaky child(ex);
    child.failures = 3;
    stepwright::retry capped(child, ex,
                             {.max_attempts = 4,
                              .initial_backoff = 100s,
                              .multiplier = std::numeric_limits<double>::max(),
                              .max_backoff = 1s});
    EXPECT_FALSE(ex.run(capped));
    EXPECT_EQ(ex.now().time_since_epoch(), 3s);
    child.calls = 0;
    child.failures = 3;
    stepwright::retry zero(child, ex,
                           {.max_attempts = 4,
                            .initial_backoff = 0s,
                            .multiplier = std::numeric_limits<double>::max()});
    EXPECT_FALSE(ex.run(zero));
    EXPECT_EQ(ex.now().time_since_epoch(), 3s);
    child.calls = 0;
    stepwright::retry fractional(
        child, ex,
        {.max_attempts = 4, .initial_backoff = 3ns, .multiplier = 1.5, .max_backoff = 100ns});
    EXPECT_FALSE(ex.run(fractional));
    EXPECT_EQ(ex.now().time_since_epoch(), 3s + 13ns); // 3, 4, 6 ticks
}

TEST(retry, debug_policy_preconditions) {
#if STEPWRIGHT_DEBUG
    stepwright::testing::virtual_executor ex;
    test_support::sync_child child;
    EXPECT_DEATH((stepwright::retry(child, ex, {.max_attempts = 0})), "at least one attempt");
    EXPECT_DEATH((stepwright::retry(child, ex, {.multiplier = 0.5})), "multiplier");
    EXPECT_DEATH(
        (stepwright::retry(child, ex, {.multiplier = std::numeric_limits<double>::infinity()})),
        "multiplier");
    EXPECT_DEATH((stepwright::retry(child, ex, {.initial_backoff = -1s})), "nonnegative");
    EXPECT_DEATH((stepwright::retry(child, ex, {.max_backoff = -1s})), "nonnegative");
    EXPECT_DEATH((stepwright::retry(child, ex, {.should_retry = nullptr})), "predicate");
#endif
}
