#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace {
// Counts every completion it receives; used to detect duplicate wakes.
struct counting_receiver {
    std::atomic<int> calls{0};
    stepwright::error result;
    void finish(const stepwright::error& e) noexcept {
        result = e;
        calls.fetch_add(1, std::memory_order_acq_rel);
    }
    stepwright::completion callback() noexcept {
        return stepwright::completion::bind<&counting_receiver::finish>(this);
    }
};

bool wait_for(const std::atomic<int>& value, int target, std::chrono::seconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (value.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
} // namespace

TEST(channel, bounded_and_unbounded_order) {
    stepwright::channel<int> bounded(3);
    EXPECT_EQ(bounded.capacity(), 3U);
    EXPECT_TRUE(bounded.try_send(1));
    EXPECT_TRUE(bounded.try_send(2));
    EXPECT_TRUE(bounded.try_send(3));
    EXPECT_FALSE(bounded.try_send(4));
    EXPECT_EQ(bounded.size(), 3U);
    EXPECT_EQ(bounded.try_pull().value(), 1);
    EXPECT_TRUE(bounded.try_send(5));
    EXPECT_EQ(bounded.try_pull().value(), 2);
    EXPECT_EQ(bounded.try_pull().value(), 3);
    EXPECT_EQ(bounded.try_pull().value(), 5);
    EXPECT_TRUE(bounded.try_pull().is_pending());

    stepwright::channel<std::unique_ptr<int>> unbounded;
    EXPECT_EQ(unbounded.capacity(), 0U);
    for (int round = 0; round < 3; ++round) {
        int pulled = 0;
        for (int i = 0; i < 1000; ++i) {
            EXPECT_TRUE(unbounded.try_send(std::make_unique<int>(i)));
            if (i % 3 == 0) {
                EXPECT_EQ(*unbounded.try_pull().value(), pulled++);
            }
        }
        std::vector<int> rest;
        for (auto next = unbounded.try_pull(); next.has_item(); next = unbounded.try_pull()) {
            rest.push_back(*next.value());
        }
        ASSERT_EQ(rest.size(), 1000U - 334U);
        EXPECT_EQ(rest.front(), 334);
        EXPECT_EQ(rest.back(), 999);
    }
    // Items left in the channel are destroyed with it.
    stepwright::channel<std::unique_ptr<int>> leftover(2);
    EXPECT_TRUE(leftover.try_send(std::make_unique<int>(1)));
}

TEST(channel, backpressure_and_fifo_senders) {
    stepwright::channel<int> ch(1);
    EXPECT_TRUE(ch.try_send(0));
    std::deque<stepwright::channel<int>::send_op> senders;
    std::vector<std::unique_ptr<test_support::receiver>> receivers;
    for (int i = 1; i <= 3; ++i) {
        senders.emplace_back(ch, i);
        receivers.push_back(std::make_unique<test_support::receiver>());
        senders.back().start(receivers.back()->callback());
        EXPECT_EQ(receivers.back()->calls, 0); // full: waits
    }
    EXPECT_FALSE(ch.try_send(99)); // no overtaking of waiting senders
    for (int i = 0; i <= 3; ++i) {
        EXPECT_EQ(ch.try_pull().value(), i);
        if (i < 3) {
            EXPECT_EQ(receivers[static_cast<std::size_t>(i)]->calls, 1); // admitted in order
            EXPECT_FALSE(receivers[static_cast<std::size_t>(i)]->result);
        }
    }
    EXPECT_TRUE(ch.try_pull().is_pending());
    // Restart a completed send_op with a new value.
    senders.front().value() = 42;
    test_support::receiver again;
    senders.front().start(again.callback());
    EXPECT_EQ(again.calls, 1);
    EXPECT_EQ(ch.try_pull().value(), 42);
}

TEST(channel, send_wakes_waiting_consumer) {
    for (const bool bounded : {false, true}) {
        auto ch = bounded ? std::make_unique<stepwright::channel<int>>(2)
                          : std::make_unique<stepwright::channel<int>>();
        test_support::receiver ready;
        ch->on_ready(ready.callback(), {});
        EXPECT_EQ(ready.calls, 0);
        auto op = ch->send(5);
        test_support::receiver sent;
        op.start(sent.callback());
        EXPECT_EQ(sent.calls, 1);
        EXPECT_EQ(ready.calls, 1);
        EXPECT_EQ(ch->try_pull().value(), 5);
    }
}

TEST(channel, close_drains_then_ends) {
    stepwright::channel<int> ch(4);
    EXPECT_TRUE(ch.try_send(1));
    EXPECT_TRUE(ch.try_send(2));
    EXPECT_FALSE(ch.is_closed());
    ch.close();
    ch.close(stepwright::error::make(stepwright::errc::timed_out)); // first reason wins
    EXPECT_TRUE(ch.is_closed());
    EXPECT_FALSE(ch.try_send(3));
    auto late = ch.send(4);
    test_support::receiver r;
    late.start(r.callback());
    EXPECT_TRUE(r.result.is(stepwright::errc::closed));
    test_support::receiver ready;
    ch.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(ch.try_pull().value(), 1);
    EXPECT_EQ(ch.try_pull().value(), 2);
    EXPECT_TRUE(ch.try_pull().at_end());
    EXPECT_TRUE(ch.try_pull().at_end());

    stepwright::channel<int> failed;
    test_support::receiver woken;
    failed.on_ready(woken.callback(), {});
    failed.close(stepwright::error::make(stepwright::errc::broken_promise));
    EXPECT_EQ(woken.calls, 1);
    EXPECT_FALSE(woken.result);
    EXPECT_TRUE(failed.try_pull().failure_reason().is(stepwright::errc::broken_promise));
}

TEST(channel, close_completes_waiting_senders) {
    stepwright::channel<std::unique_ptr<int>> ch(1);
    EXPECT_TRUE(ch.try_send(std::make_unique<int>(0)));
    auto a = ch.send(std::make_unique<int>(1));
    auto b = ch.send(std::make_unique<int>(2));
    test_support::receiver ra;
    test_support::receiver rb;
    a.start(ra.callback());
    b.start(rb.callback());
    ch.close();
    EXPECT_TRUE(ra.result.is(stepwright::errc::closed));
    EXPECT_TRUE(rb.result.is(stepwright::errc::closed));
    ASSERT_TRUE(a.value());
    EXPECT_EQ(*a.value(), 1); // dropped by the channel, still owned by the op
    EXPECT_EQ(*ch.try_pull().value(), 0);
    EXPECT_TRUE(ch.try_pull().at_end());
}

TEST(channel, cancel_waiting_sender_and_consumer) {
    stepwright::channel<int> ch(1);
    EXPECT_TRUE(ch.try_send(0));
    stepwright::cancel_source source;
    auto first = ch.send(1);
    auto second = ch.send(2);
    test_support::receiver r1;
    test_support::receiver r2;
    first.start(r1.callback(), source.token());
    second.start(r2.callback());
    source.request_cancel();
    EXPECT_TRUE(r1.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(first.value(), 1);
    EXPECT_EQ(ch.try_pull().value(), 0);
    EXPECT_EQ(r2.calls, 1); // the cancelled sender left the queue
    EXPECT_EQ(ch.try_pull().value(), 2);
    // Already-cancelled token while full: queued, then cancelled during arming.
    EXPECT_TRUE(ch.try_send(3));
    test_support::receiver r3;
    first.start(r3.callback(), source.token());
    EXPECT_TRUE(r3.result.is(stepwright::errc::cancelled));

    stepwright::cancel_source consumer;
    test_support::receiver ready;
    EXPECT_EQ(ch.try_pull().value(), 3);
    ch.on_ready(ready.callback(), consumer.token());
    consumer.request_cancel();
    EXPECT_TRUE(ready.result.is(stepwright::errc::cancelled));
    EXPECT_TRUE(ch.try_send(4)); // no stale wake
    EXPECT_EQ(ch.try_pull().value(), 4);
    test_support::receiver pre;
    ch.on_ready(pre.callback(), consumer.token()); // already cancelled
    EXPECT_TRUE(pre.result.is(stepwright::errc::cancelled));
}

TEST(channel, regression_concurrent_senders_wake_once) {
    const int producers = 8;
    const int per_producer = test_support::stress_iterations(2'000);
    for (const bool bounded : {false, true}) {
        auto ch = bounded ? std::make_unique<stepwright::channel<int>>(4)
                          : std::make_unique<stepwright::channel<int>>();
        std::vector<std::thread> threads;
        threads.reserve(producers);
        for (int p = 0; p < producers; ++p) {
            threads.emplace_back([&, p] {
                unsigned seed = static_cast<unsigned>(p) + 1;
                for (int i = 0; i < per_producer; ++i) {
                    const int value = (p * per_producer) + i;
                    if (!bounded || i % 4 == 0) {
                        test_support::jitter(seed, 50); // let the consumer run dry and wait
                    }
                    if (bounded) {
                        auto op = ch->send(value);
                        test_support::receiver sent;
                        op.start(sent.callback());
                        sent.wait();
                        ASSERT_FALSE(sent.result);
                    } else {
                        ASSERT_TRUE(ch->try_send(value));
                    }
                }
            });
        }
        std::vector<int> next(producers, 0);
        int received = 0;
        int waits = 0;
        counting_receiver ready;
        while (received < producers * per_producer) {
            auto item = ch->try_pull();
            if (item.has_item()) {
                const int value = item.value();
                const int p = value / per_producer;
                ASSERT_EQ(value % per_producer, next[static_cast<std::size_t>(p)]++);
                ++received;
                continue;
            }
            ASSERT_TRUE(item.is_pending());
            ++waits;
            ch->on_ready(ready.callback(), {});
            ASSERT_TRUE(wait_for(ready.calls, waits)) << "lost wake-up";
        }
        for (auto& thread : threads) {
            thread.join();
        }
        std::this_thread::sleep_for(1ms);
        EXPECT_EQ(ready.calls.load(), waits); // exactly one wake per wait
        EXPECT_TRUE(ch->try_pull().is_pending());
        std::cout << (bounded ? "bounded" : "unbounded") << ": waits=" << waits << '\n';
    }
}

TEST(channel, regression_send_vs_consumer_wait) {
    const int rounds = test_support::stress_iterations(20'000);
    stepwright::channel<int> ch;
    std::atomic<int> round_value{0};
    test_support::race_rounds race({[&] {
        static thread_local unsigned seed = 1;
        test_support::jitter(seed, 2);
        ASSERT_TRUE(ch.try_send(round_value.load(std::memory_order_relaxed)));
    }});
    unsigned seed = 7;
    int waited = 0;
    for (int r = 0; r < rounds; ++r) {
        round_value.store(r, std::memory_order_relaxed);
        counting_receiver ready;
        bool registered = false;
        std::optional<int> early;
        race.run_round([&] {
            test_support::jitter(seed, 2);
            auto item = ch.try_pull();
            if (item.has_item()) {
                early = item.value();
            } else {
                ch.on_ready(ready.callback(), {});
                registered = true;
            }
        });
        if (early) {
            ASSERT_EQ(*early, r);
            continue;
        }
        ASSERT_TRUE(registered);
        ++waited;
        ASSERT_TRUE(wait_for(ready.calls, 1)) << "lost wake-up in round " << r;
        ASSERT_EQ(ch.try_pull().value(), r);
        ASSERT_EQ(ready.calls.load(), 1);
    }
    std::cout << "waited " << waited << " / " << rounds << '\n';
}

TEST(channel, regression_consumer_destroyed_after_cancel) {
    stepwright::channel<int> ch;
    std::atomic<bool> stop{false};
    std::atomic<long long> sent{0};
    std::vector<std::thread> producers;
    producers.reserve(4);
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (ch.try_send(1)) {
                    sent.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });
    }
    std::atomic<long long> received{0};
    const auto count = [&](int v) { received.fetch_add(v, std::memory_order_relaxed); };
    const int rounds = test_support::stress_iterations(500);
    for (int r = 0; r < rounds; ++r) {
        stepwright::cancel_source source;
        auto consumer = std::make_unique<stepwright::for_each_op<int, decltype(count)>>(ch, count);
        test_support::receiver done;
        // Producers can keep the consumer busy inside start(): cancel elsewhere.
        std::thread canceller([&] {
            std::this_thread::yield();
            source.request_cancel();
        });
        consumer->start(done.callback(), source.token());
        canceller.join();
        done.wait();
        ASSERT_TRUE(done.result.is(stepwright::errc::cancelled));
        consumer.reset();
    }
    stop.store(true, std::memory_order_release);
    for (auto& thread : producers) {
        thread.join();
    }
    ch.close();
    for (auto next = ch.try_pull(); next.has_item(); next = ch.try_pull()) {
        received.fetch_add(next.value());
    }
    EXPECT_EQ(received.load(), sent.load());
}

TEST(channel, regression_close_vs_send) {
    const int rounds = test_support::stress_iterations(5'000);
    std::optional<stepwright::channel<int>> ch;
    std::optional<stepwright::channel<int>::send_op> waiting;
    counting_receiver sender;
    std::atomic<bool> accepted{false};
    int try_send_won = 0;
    int waiting_admitted = 0;
    test_support::race_rounds race({
        [&] { accepted.store(ch->try_send(1)); }, // unbounded producer
        [&] { ch->close(); },                     // closer
        [&] {                                     // consumer (admits the waiter)
            static_cast<void>(ch->try_pull());
            static_cast<void>(ch->try_pull());
        },
    });
    for (int r = 0; r < rounds; ++r) {
        ch.emplace(2);
        waiting.reset();
        EXPECT_TRUE(ch->try_send(0));
        EXPECT_TRUE(ch->try_send(5));
        waiting.emplace(*ch, 2);
        sender.calls.store(0);
        waiting->start(sender.callback()); // full: waits
        race.run_round();
        ASSERT_EQ(sender.calls.load(), 1); // completed exactly once, synchronously
        const bool admitted = !sender.result;
        if (!admitted) {
            ASSERT_TRUE(sender.result.is(stepwright::errc::closed));
        }
        std::vector<int> rest = stream_support::drain<int>(*ch);
        ASSERT_TRUE(ch->try_pull().at_end());
        // The consumer took 0 and 5 (buffered first); what remains is exactly the
        // admitted waiter's value and an accepted try_send.
        const auto has = [&](int v) { return std::ranges::find(rest, v) != rest.end(); };
        ASSERT_EQ(has(2), admitted);
        ASSERT_EQ(has(1), accepted.load());
        try_send_won += accepted.load() ? 1 : 0;
        waiting_admitted += admitted ? 1 : 0;
    }
    waiting.reset();
    std::cout << "admitted " << waiting_admitted << ", try_send accepted " << try_send_won << " / "
              << rounds << '\n';
}
