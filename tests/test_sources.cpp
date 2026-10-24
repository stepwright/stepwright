#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <chrono>
#include <list>
#include <string>

using namespace std::chrono_literals;

namespace {
template <class R>
concept from_range_accepts = requires(R&& r) { stepwright::from_range(std::forward<R>(r)); };
} // namespace

TEST(sources, empty_and_just) {
    auto none = stepwright::empty<int>();
    EXPECT_TRUE(none.try_pull().at_end());
    test_support::receiver ready;
    none.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 1);
    auto values = stepwright::just(std::string("a"), "b", std::string("c"));
    static_assert(std::is_same_v<decltype(values)::value_type, std::string>);
    EXPECT_EQ(stream_support::drain<std::string>(values),
              (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_TRUE(values.try_pull().at_end());
    values.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 2);
}

TEST(sources, from_range_copies_without_owning) {
    std::list<int> numbers{1, 2, 3};
    auto s = stepwright::from_range(numbers);
    EXPECT_EQ(stream_support::drain<int>(s), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(numbers.size(), 3U);
    EXPECT_TRUE(s.try_pull().at_end());
    const std::vector<std::string> words{"x", "y"};
    auto w = stepwright::from_range(words);
    test_support::receiver ready;
    w.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(stream_support::drain<std::string>(w), words);
    static_assert(from_range_accepts<std::vector<int>&>);
    static_assert(!from_range_accepts<std::vector<int>>);
}

TEST(sources, generate_until_nullopt) {
    int calls = 0;
    auto g = stepwright::generate([&]() -> std::optional<int> {
        ++calls;
        return calls <= 3 ? std::optional<int>(calls * 10) : std::nullopt;
    });
    EXPECT_EQ(stream_support::drain<int>(g), (std::vector<int>{10, 20, 30}));
    EXPECT_TRUE(g.try_pull().at_end());
    EXPECT_EQ(calls, 4); // not called again after nullopt
    test_support::receiver ready;
    g.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 1);
}

TEST(sources, interval_with_manual_timers) {
    stepwright::testing::manual_timer_service timers;
    auto ticks = stepwright::interval(timers, 1s);
    EXPECT_EQ(ticks.period(), 1s);
    EXPECT_TRUE(ticks.try_pull().is_pending());
    test_support::receiver ready;
    ticks.on_ready(ready.callback(), {});
    EXPECT_EQ(timers.pending(), 1U);
    timers.advance(999ms);
    EXPECT_EQ(ready.calls, 0);
    timers.advance(1ms);
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(ticks.try_pull().value(), 0U);
    EXPECT_TRUE(ticks.try_pull().is_pending());
    timers.advance(1s);
    EXPECT_EQ(ticks.try_pull().value(), 1U);
    // Missed ticks coalesce into the latest one.
    timers.advance(3500ms);
    test_support::receiver now;
    ticks.on_ready(now.callback(), {});
    EXPECT_EQ(now.calls, 1); // already due: synchronous, no timer
    EXPECT_EQ(timers.pending(), 0U);
    EXPECT_EQ(ticks.try_pull().value(), 4U);
    EXPECT_TRUE(ticks.try_pull().is_pending());
    // Cancelling a readiness wait removes the timer entry.
    stepwright::cancel_source consumer;
    test_support::receiver cancelled;
    ticks.on_ready(cancelled.callback(), consumer.token());
    EXPECT_EQ(timers.pending(), 1U);
    consumer.request_cancel();
    EXPECT_TRUE(cancelled.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(timers.pending(), 0U);
    timers.advance(1s);
    EXPECT_EQ(ticks.try_pull().value(), 5U);
}

TEST(sources, interval_with_steady_timers) {
    stepwright::steady_timer_service timers;
    auto ticks = stepwright::interval(timers, 1ms);
    std::uint64_t last = 0;
    for (int i = 0; i < 5; ++i) {
        auto next = stream_support::pull_blocking<std::uint64_t>(ticks);
        ASSERT_TRUE(next.has_item());
        if (i != 0) {
            EXPECT_GT(next.value(), last);
        }
        last = next.value();
    }
    // Fire vs cancellation race on the timer thread.
    const int rounds = test_support::stress_iterations(500);
    int fired = 0;
    unsigned seed = 3;
    for (int i = 0; i < rounds; ++i) {
        stepwright::cancel_source consumer;
        test_support::receiver r;
        if (!ticks.try_pull().is_pending()) {
            continue;
        }
        ticks.on_ready(r.callback(), consumer.token());
        test_support::jitter(seed, 1000);
        consumer.request_cancel();
        r.wait();
        fired += r.result ? 0 : 1;
    }
    std::cout << "fired " << fired << " / " << rounds << '\n';
}
