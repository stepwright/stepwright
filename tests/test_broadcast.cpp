#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <deque>
#include <memory>
#include <optional>
#include <thread>

namespace {
using sub_t = stepwright::broadcast<int>::subscription;
std::vector<int> drain_now(sub_t& s) {
    std::vector<int> items;
    for (auto next = s.try_pull(); next.has_item(); next = s.try_pull()) {
        items.push_back(next.value());
    }
    return items;
}
} // namespace

TEST(broadcast, every_subscriber_sees_every_item_in_order) {
    stepwright::broadcast<int> b(8);
    EXPECT_EQ(b.capacity(), 8U);
    auto s1 = b.subscribe();
    auto s2 = b.subscribe();
    auto s3 = b.subscribe();
    EXPECT_EQ(b.subscribers(), 3U);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(b.try_publish(i));
    }
    const std::vector<int> expected{0, 1, 2, 3, 4};
    EXPECT_EQ(drain_now(s1), expected);
    EXPECT_EQ(drain_now(s2), expected);
    EXPECT_EQ(drain_now(s3), expected);
    EXPECT_TRUE(s1.try_pull().is_pending());
}

TEST(broadcast, late_subscriber_starts_at_next_item) {
    stepwright::broadcast<int> b(4);
    EXPECT_TRUE(b.try_publish(-1)); // no subscriber: discarded, never waits
    auto early = b.subscribe();
    EXPECT_TRUE(b.try_publish(1));
    auto late = b.subscribe();
    EXPECT_TRUE(b.try_publish(2));
    EXPECT_EQ(drain_now(early), (std::vector<int>{1, 2}));
    EXPECT_EQ(drain_now(late), (std::vector<int>{2}));
}

TEST(broadcast, wait_backpressure_releases_when_slowest_catches_up) {
    stepwright::broadcast<int> b(2);
    auto fast = b.subscribe();
    auto slow = b.subscribe();
    EXPECT_TRUE(b.try_publish(1));
    EXPECT_TRUE(b.try_publish(2));
    EXPECT_EQ(drain_now(fast), (std::vector<int>{1, 2}));
    EXPECT_FALSE(b.try_publish(3)); // slow still holds the ring
    auto op = b.publish(3);
    test_support::receiver published;
    op.start(published.callback());
    EXPECT_EQ(published.calls, 0);
    EXPECT_FALSE(b.try_publish(4)); // a publish is waiting
    test_support::receiver fast_ready;
    fast.on_ready(fast_ready.callback(), {});
    EXPECT_EQ(fast_ready.calls, 0);
    EXPECT_EQ(slow.try_pull().value(), 1); // frees the oldest slot
    EXPECT_EQ(published.calls, 1);
    EXPECT_FALSE(published.result);
    EXPECT_EQ(fast_ready.calls, 1); // woken by the admitted item
    EXPECT_EQ(fast.try_pull().value(), 3);
    EXPECT_EQ(drain_now(slow), (std::vector<int>{2, 3}));
}

TEST(broadcast, drop_oldest_reports_gaps) {
    stepwright::broadcast<int> b(2, stepwright::overflow::drop_oldest);
    auto s = b.subscribe();
    auto keeping_up = b.subscribe();
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(b.try_publish(i));
        EXPECT_EQ(keeping_up.try_pull().value(), i);
    }
    EXPECT_EQ(drain_now(s), (std::vector<int>{3, 4}));
    EXPECT_EQ(s.dropped(), 3U);
    EXPECT_EQ(keeping_up.dropped(), 0U);
    auto op = b.publish(5); // never waits
    test_support::receiver r;
    op.start(r.callback());
    EXPECT_EQ(r.calls, 1);
    for (int i = 6; i < 9; ++i) {
        EXPECT_TRUE(b.try_publish(i));
    }
    test_support::receiver ready;
    s.on_ready(ready.callback(), {}); // catches up past the gap, item available
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(s.dropped(), 5U);
    EXPECT_EQ(drain_now(s), (std::vector<int>{7, 8}));
    EXPECT_EQ(drain_now(keeping_up), (std::vector<int>{7, 8}));
    EXPECT_EQ(keeping_up.dropped(), 2U);
}

TEST(broadcast, unsubscribing_the_slowest_releases_publisher) {
    stepwright::broadcast<int> b(1);
    auto fast = b.subscribe();
    auto slow = std::make_unique<sub_t>(b);
    EXPECT_TRUE(b.try_publish(1));
    EXPECT_EQ(fast.try_pull().value(), 1);
    auto op = b.publish(2);
    test_support::receiver published;
    op.start(published.callback());
    EXPECT_EQ(published.calls, 0);
    slow.reset();
    EXPECT_EQ(published.calls, 1);
    EXPECT_EQ(b.subscribers(), 1U);
    EXPECT_EQ(fast.try_pull().value(), 2);
}

TEST(broadcast, close_cancel_and_readiness) {
    stepwright::broadcast<int> b(1);
    auto s = b.subscribe();
    test_support::receiver ready;
    stepwright::cancel_source consumer;
    s.on_ready(ready.callback(), consumer.token());
    consumer.request_cancel();
    EXPECT_TRUE(ready.result.is(stepwright::errc::cancelled));
    test_support::receiver woken;
    s.on_ready(woken.callback(), {});
    EXPECT_TRUE(b.try_publish(1));
    EXPECT_EQ(woken.calls, 1);
    stepwright::cancel_source producer;
    auto op = b.publish(2);
    test_support::receiver cancelled;
    op.start(cancelled.callback(), producer.token());
    producer.request_cancel();
    EXPECT_TRUE(cancelled.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(op.value(), 2);
    test_support::receiver closed;
    op.start(closed.callback());
    EXPECT_EQ(closed.calls, 0);
    b.close(stepwright::error::make(stepwright::errc::broken_promise));
    b.close();
    EXPECT_TRUE(b.is_closed());
    EXPECT_TRUE(closed.result.is(stepwright::errc::closed));
    EXPECT_FALSE(b.try_publish(3));
    test_support::receiver after;
    op.start(after.callback());
    EXPECT_TRUE(after.result.is(stepwright::errc::closed));
    EXPECT_EQ(s.try_pull().value(), 1); // drains, then the close reason
    EXPECT_TRUE(s.try_pull().failure_reason().is(stepwright::errc::broken_promise));
    test_support::receiver done;
    s.on_ready(done.callback(), {});
    EXPECT_EQ(done.calls, 1);
    auto other = b.subscribe();
    test_support::receiver waiting;
    stepwright::broadcast<int> b2(1);
    auto s2 = b2.subscribe();
    s2.on_ready(waiting.callback(), {});
    b2.close();
    EXPECT_EQ(waiting.calls, 1);
    EXPECT_TRUE(s2.try_pull().at_end());
    EXPECT_TRUE(other.try_pull().is_failed());
}

TEST(broadcast, one_thousand_subscribers) {
    constexpr int subscribers = 1000;
    constexpr int items = 100;
    stepwright::broadcast<int> b(16);
    std::deque<sub_t> subs;
    for (int i = 0; i < subscribers; ++i) {
        subs.emplace_back(b);
    }
    std::vector<int> next(subscribers, 0);
    for (int i = 0; i < items; ++i) {
        auto op = b.publish(i);
        test_support::receiver published;
        op.start(published.callback());
        while (published.calls == 0) {
            // Readers catch up only as far as the ring allows; the publish waits
            // for the slowest of them.
            for (std::size_t k = 0; k < subs.size(); ++k) {
                auto item = subs[k].try_pull();
                if (item.has_item()) {
                    ASSERT_EQ(item.value(), next[k]++);
                }
            }
        }
    }
    for (std::size_t k = 0; k < subs.size(); ++k) {
        for (int v : drain_now(subs[k])) {
            ASSERT_EQ(v, next[k]++);
        }
        ASSERT_EQ(next[k], items);
    }
}

TEST(broadcast, concurrent_subscribe_unsubscribe_stress) {
    const int items = test_support::stress_iterations(20'000);
    for (const auto policy : {stepwright::overflow::wait, stepwright::overflow::drop_oldest}) {
        stepwright::broadcast<int> b(8, policy);
        std::atomic<bool> producer_done{false};
        std::atomic<long> observed{0};
        std::vector<std::thread> churn;
        churn.reserve(4);
        for (int t = 0; t < 4; ++t) {
            churn.emplace_back([&, t] {
                unsigned seed = static_cast<unsigned>(t) + 1;
                while (!producer_done.load(std::memory_order_acquire)) {
                    sub_t s(b);
                    seed = seed * 1664525U + 1013904223U;
                    const int reads = static_cast<int>((seed >> 16) % 16);
                    int last = -1;
                    for (int k = 0; k < reads; ++k) {
                        auto item = stream_support::pull_blocking<int>(s);
                        if (!item.has_item()) {
                            break;
                        }
                        // In order; consecutive unless items were dropped.
                        ASSERT_GT(item.value(), last);
                        if (policy == stepwright::overflow::wait && last >= 0) {
                            ASSERT_EQ(item.value(), last + 1);
                        }
                        last = item.value();
                        observed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (int i = 0; i < items; ++i) {
            auto op = b.publish(i);
            test_support::receiver published;
            op.start(published.callback());
            published.wait();
            ASSERT_FALSE(published.result);
        }
        b.close();
        producer_done.store(true, std::memory_order_release);
        for (auto& thread : churn) {
            thread.join();
        }
        EXPECT_EQ(b.subscribers(), 0U);
        std::cout << "observed " << observed.load() << " deliveries\n";
    }
}

TEST(broadcast, regression_publish_vs_subscriber_cancel) {
    const int rounds = test_support::stress_iterations(20'000);
    stepwright::broadcast<int> b(4);
    sub_t s(b);
    std::optional<stepwright::cancel_source> consumer;
    unsigned seed_a = 1;
    unsigned seed_b = 2;
    int next = 0;
    test_support::race_rounds race({
        [&] {
            test_support::jitter(seed_a, 3);
            EXPECT_TRUE(b.try_publish(next));
        },
        [&] {
            test_support::jitter(seed_b, 3);
            consumer->request_cancel();
        },
    });
    int cancelled = 0;
    for (; next < rounds; ++next) {
        consumer.emplace();
        test_support::receiver ready;
        s.on_ready(ready.callback(), consumer->token());
        race.run_round();
        ready.wait();
        ASSERT_EQ(ready.calls, 1);
        cancelled += ready.result ? 1 : 0;
        ASSERT_EQ(s.try_pull().value(), next);
    }
    std::cout << "cancelled " << cancelled << " / " << rounds << '\n';
}
