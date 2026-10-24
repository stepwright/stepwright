#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace {
auto constant(int value) {
    return stepwright::generate([value]() -> std::optional<int> { return value; });
}
} // namespace

TEST(merge, fair_rotation) {
    auto a = constant(0);
    auto b = constant(1);
    auto c = constant(2);
    stepwright::merge m(a, b, c);
    static_assert(std::is_same_v<decltype(m), stepwright::merge<int, 3>>);
    std::vector<int> order;
    order.reserve(9);
    for (int i = 0; i < 9; ++i) {
        order.push_back(m.try_pull().value());
    }
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 0, 1, 2, 0, 1, 2}));
    stepwright::merge from_array(std::array<stepwright::source<int>*, 2>{&a, &b});
    EXPECT_EQ(from_array.try_pull().value(), 0);
    EXPECT_EQ(from_array.try_pull().value(), 1);
}

TEST(merge, ends_when_all_inputs_end) {
    auto a = stepwright::just(1, 2);
    auto b = stepwright::empty<int>();
    auto c = stepwright::just(3);
    stepwright::merge m(a, b, c);
    test_support::receiver ready;
    m.on_ready(ready.callback(), {}); // inputs ready: completes synchronously
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(stream_support::drain<int>(m), (std::vector<int>{1, 3, 2}));
    EXPECT_TRUE(m.try_pull().at_end());
    test_support::receiver done;
    m.on_ready(done.callback(), {});
    EXPECT_EQ(done.calls, 1);
}

TEST(merge, first_failure_is_sticky_and_cancels_other_waits) {
    stream_support::probe_source<int> a;
    stream_support::probe_source<int> b;
    stream_support::probe_source<int> c;
    stepwright::merge m(a, b, c);
    EXPECT_TRUE(m.try_pull().is_pending());
    test_support::receiver ready;
    m.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 0);
    EXPECT_EQ(a.waits.load() + b.waits.load() + c.waits.load(), 3);
    b.inner.close(stepwright::error::make(stepwright::errc::broken_promise));
    EXPECT_EQ(ready.calls, 1);
    EXPECT_FALSE(ready.result);
    EXPECT_EQ(a.cancelled.load(), 1);
    EXPECT_EQ(c.cancelled.load(), 1);
    EXPECT_EQ(b.cancelled.load(), 0);
    auto failed = m.try_pull();
    ASSERT_TRUE(failed.is_failed());
    EXPECT_TRUE(failed.failure_reason().is(stepwright::errc::broken_promise));
    EXPECT_TRUE(c.inner.try_send(5));
    EXPECT_TRUE(m.try_pull().is_failed()); // no input is polled afterwards
    EXPECT_EQ(c.inner.size(), 1U);
    test_support::receiver after;
    m.on_ready(after.callback(), {});
    EXPECT_EQ(after.calls, 1);
}

TEST(merge, consumer_cancellation_cancels_the_round) {
    stream_support::probe_source<int> a;
    stream_support::probe_source<int> b;
    stepwright::merge m(a, b);
    for (int round = 0; round < 3; ++round) {
        stepwright::cancel_source consumer;
        test_support::receiver ready;
        EXPECT_TRUE(m.try_pull().is_pending());
        m.on_ready(ready.callback(), consumer.token());
        consumer.request_cancel();
        EXPECT_TRUE(ready.result.is(stepwright::errc::cancelled));
        EXPECT_EQ(a.cancelled.load(), round + 1);
        EXPECT_EQ(b.cancelled.load(), round + 1);
    }
    // An already-cancelled token with an input ready: the round completes first.
    stepwright::cancel_source cancelled;
    cancelled.request_cancel();
    EXPECT_TRUE(b.inner.try_send(7));
    test_support::receiver ready;
    m.on_ready(ready.callback(), cancelled.token());
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(m.try_pull().value(), 7);
    // Restart after cancellation: a later send completes the next round.
    test_support::receiver next;
    stepwright::cancel_source live;
    m.on_ready(next.callback(), live.token());
    EXPECT_TRUE(a.inner.try_send(8));
    EXPECT_EQ(next.calls, 1);
    EXPECT_FALSE(next.result);
    EXPECT_EQ(m.try_pull().value(), 8);
}

TEST(merge, regression_no_head_of_line_blocking) {
    stepwright::testing::manual_timer_service timers;
    const auto start = timers.now();
    auto slow = stepwright::interval(timers, 10s);
    auto fast = stepwright::interval(timers, 1s);
    stepwright::merge m(slow, fast);
    std::vector<stepwright::timer_service::clock::duration> at;
    auto consumer = stepwright::for_each(m, [&](std::uint64_t) {
        at.push_back(timers.now() - start);
        return at.size() < 2;
    });
    test_support::receiver done;
    consumer.start(done.callback());
    for (int step = 0; step < 25 && done.calls == 0; ++step) {
        timers.advance(100ms);
    }
    EXPECT_EQ(done.calls, 1);
    EXPECT_FALSE(done.result);
    ASSERT_EQ(at.size(), 2U);
    EXPECT_EQ(at[0], 1s);
    EXPECT_EQ(at[1], 2s);
    EXPECT_EQ(timers.pending(), 0U); // the slow input's wait was cancelled each round
}

TEST(merge, inputs_completing_on_other_threads) {
    const int inputs = 4;
    const int per_input = test_support::stress_iterations(5'000);
    std::vector<std::unique_ptr<stepwright::channel<int>>> channels;
    std::vector<stepwright::source<int>*> sources;
    for (int i = 0; i < inputs; ++i) {
        // Mix bounded (backpressure) and unbounded inputs.
        channels.push_back(i % 2 == 0 ? std::make_unique<stepwright::channel<int>>(4)
                                      : std::make_unique<stepwright::channel<int>>());
        sources.push_back(channels.back().get());
    }
    stepwright::merge_range<int> m(sources);
    std::vector<int> next(inputs, 0);
    int received = 0;
    bool ordered = true;
    auto consumer = stepwright::for_each(m, [&](int value) {
        const int input = value / per_input;
        ordered = ordered && value % per_input == next[static_cast<std::size_t>(input)]++;
        ++received;
    });
    test_support::receiver done;
    consumer.start(done.callback());
    std::vector<std::thread> producers;
    producers.reserve(inputs);
    for (int i = 0; i < inputs; ++i) {
        producers.emplace_back([&, i] {
            auto& ch = *channels[static_cast<std::size_t>(i)];
            for (int k = 0; k < per_input; ++k) {
                auto op = ch.send((i * per_input) + k);
                test_support::receiver sent;
                op.start(sent.callback());
                sent.wait();
            }
            ch.close();
        });
    }
    for (auto& thread : producers) {
        thread.join();
    }
    done.wait();
    EXPECT_FALSE(done.result);
    EXPECT_TRUE(ordered);
    EXPECT_EQ(received, inputs * per_input);
}

TEST(merge, sixty_four_input_range) {
    const std::size_t count = 64;
    std::vector<std::unique_ptr<stream_support::probe_source<int>>> probes;
    std::vector<stepwright::source<int>*> sources;
    for (std::size_t i = 0; i < count; ++i) {
        probes.push_back(std::make_unique<stream_support::probe_source<int>>());
        sources.push_back(probes.back().get());
    }
    stepwright::merge_range<int> m(sources);
    EXPECT_TRUE(m.try_pull().is_pending());
    test_support::receiver ready;
    m.on_ready(ready.callback(), {});
    std::thread producer([&] { EXPECT_TRUE(probes[37]->inner.try_send(37)); });
    producer.join();
    ready.wait();
    EXPECT_FALSE(ready.result);
    EXPECT_EQ(m.try_pull().value(), 37);
    int cancelled = 0;
    for (const auto& probe : probes) {
        EXPECT_EQ(probe->waits.load(), 1);
        cancelled += probe->cancelled.load();
    }
    EXPECT_EQ(cancelled, 63);
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_TRUE(probes[i]->inner.try_send(static_cast<int>(i)));
        probes[i]->inner.close();
    }
    std::vector<int> items;
    for (auto next = stream_support::pull_blocking<int>(m); next.has_item();
         next = stream_support::pull_blocking<int>(m)) {
        items.push_back(next.value());
    }
    EXPECT_EQ(items.size(), count);
    EXPECT_TRUE(m.try_pull().at_end());
}

TEST(merge, regression_round_cancel_vs_input_readiness) {
    // The consumer's cancellation races with an input becoming ready on another
    // thread; exactly one completion, never before both input waits completed.
    const int rounds = test_support::stress_iterations(20'000);
    stream_support::probe_source<int> a;
    stream_support::probe_source<int> b;
    stepwright::merge m(a, b);
    std::optional<stepwright::cancel_source> consumer;
    unsigned seed_a = 1;
    unsigned seed_b = 2;
    test_support::race_rounds race({
        [&] {
            test_support::jitter(seed_a, 3);
            EXPECT_TRUE(a.inner.try_send(1));
        },
        [&] {
            test_support::jitter(seed_b, 3);
            consumer->request_cancel();
        },
    });
    int cancelled = 0;
    for (int r = 0; r < rounds; ++r) {
        consumer.emplace();
        test_support::receiver ready;
        ASSERT_TRUE(m.try_pull().is_pending());
        m.on_ready(ready.callback(), consumer->token());
        race.run_round();
        ready.wait();
        ASSERT_EQ(ready.calls, 1);
        if (ready.result) {
            ASSERT_TRUE(ready.result.is(stepwright::errc::cancelled));
            ++cancelled;
        }
        ASSERT_EQ(m.try_pull().value(), 1); // the item is never lost
    }
    EXPECT_EQ(a.waits.load(), rounds);
    EXPECT_EQ(b.cancelled.load(), rounds); // b's wait was always cancelled
    std::cout << "cancelled " << cancelled << " / " << rounds << '\n';
}
