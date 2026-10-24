#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <memory>

TEST(for_each, drains_until_end) {
    auto values = stepwright::just(1, 2, 3);
    std::vector<int> seen;
    auto op = stepwright::for_each(values, [&](int v) { seen.push_back(v); });
    test_support::receiver r;
    op.start(r.callback());
    EXPECT_EQ(r.calls, 1);
    EXPECT_FALSE(r.result);
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
    test_support::receiver again; // restart on an ended source
    op.start(again.callback());
    EXPECT_EQ(again.calls, 1);
    op.function()(4);
    EXPECT_EQ(seen.size(), 4U);
}

TEST(for_each, waits_for_readiness) {
    stepwright::channel<int> ch;
    int sum = 0;
    auto op = stepwright::for_each(ch, [&](int v) { sum += v; });
    test_support::receiver r;
    op.start(r.callback());
    EXPECT_EQ(r.calls, 0);
    for (int i = 1; i <= 10; ++i) {
        EXPECT_TRUE(ch.try_send(i));
    }
    ch.close();
    EXPECT_EQ(r.calls, 1);
    EXPECT_FALSE(r.result);
    EXPECT_EQ(sum, 55);
}

TEST(for_each, failure_and_callback_results) {
    stepwright::channel<int> ch;
    EXPECT_TRUE(ch.try_send(1));
    ch.close(stepwright::error::make(stepwright::errc::broken_promise));
    auto op = stepwright::for_each(ch, [](int) {});
    test_support::receiver r;
    op.start(r.callback());
    EXPECT_TRUE(r.result.is(stepwright::errc::broken_promise));

    auto values = stepwright::just(1, 2, 3, 4);
    int seen = 0;
    auto stop = stepwright::for_each(values, [&](int v) {
        ++seen;
        return v < 2;
    });
    test_support::receiver stopped;
    stop.start(stopped.callback());
    EXPECT_FALSE(stopped.result);
    EXPECT_EQ(seen, 2);

    auto more = stepwright::just(1, 2, 3);
    auto failing = stepwright::for_each(more, [](int v) {
        return v == 2 ? stepwright::error::make(stepwright::errc::timed_out) : stepwright::error{};
    });
    test_support::receiver failed;
    failing.start(failed.callback());
    EXPECT_TRUE(failed.result.is(stepwright::errc::timed_out));
    EXPECT_EQ(more.try_pull().value(), 3);
}

TEST(for_each, cancellation_cancels_the_readiness_wait_first) {
    stream_support::probe_source<int> probe;
    int items = 0;
    auto op = stepwright::for_each(probe, [&](int) { ++items; });
    stepwright::cancel_source source;
    test_support::receiver r;
    op.start(r.callback(), source.token());
    EXPECT_EQ(probe.waits.load(), 1);
    source.request_cancel();
    EXPECT_EQ(probe.cancelled.load(), 1);
    EXPECT_TRUE(r.result.is(stepwright::errc::cancelled));
    EXPECT_TRUE(probe.inner.try_send(1)); // the wait is gone: nothing runs
    EXPECT_EQ(items, 0);

    // Cancelled between items: the token is checked before each pull.
    auto endless = stepwright::generate([]() -> std::optional<int> { return 1; });
    stepwright::cancel_source between;
    auto counting = stepwright::for_each(endless, [&](int) {
        if (++items == 100) {
            between.request_cancel();
        }
    });
    test_support::receiver cancelled;
    counting.start(cancelled.callback(), between.token());
    EXPECT_TRUE(cancelled.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(items, 100);
}

TEST(for_each, regression_stack_one_million_generated_items) {
    const int count = 1'000'000;
    int produced = 0;
    test_support::depth_probe probe;
    auto g = stepwright::generate([&]() -> std::optional<int> {
        probe.sample();
        if (produced == count) {
            return std::nullopt;
        }
        return produced++;
    });
    long long sum = 0;
    auto op = stepwright::for_each(g, [&](int v) {
        probe.sample();
        sum += v;
    });
    test_support::receiver r;
    op.start(r.callback());
    EXPECT_EQ(r.calls, 1);
    EXPECT_EQ(sum, static_cast<long long>(count) * (count - 1) / 2);
    EXPECT_LT(probe.span(), 64U * 1024U);
}

TEST(for_each, regression_stack_one_million_channel_wakes) {
    // Two consumers ping-pong through two channels: every item is sent while the
    // other consumer waits, so each one is a trampoline-delivered readiness wake.
    const int count = 1'000'000;
    stepwright::channel<int> a;
    stepwright::channel<int> b;
    test_support::depth_probe probe;
    int hops = 0;
    auto first = stepwright::for_each(a, [&](int v) {
        probe.sample();
        if (v >= count) {
            a.close();
            b.close();
            return;
        }
        EXPECT_TRUE(b.try_send(v + 1));
    });
    auto second = stepwright::for_each(b, [&](int v) {
        probe.sample();
        ++hops;
        EXPECT_TRUE(a.try_send(v + 1));
    });
    test_support::receiver r1;
    test_support::receiver r2;
    first.start(r1.callback());
    second.start(r2.callback());
    EXPECT_TRUE(a.try_send(0));
    EXPECT_EQ(r1.calls, 1);
    EXPECT_EQ(r2.calls, 1);
    EXPECT_EQ(hops, count / 2);
    EXPECT_LT(probe.span(), 64U * 1024U);
}
