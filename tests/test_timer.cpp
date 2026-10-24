#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using stepwright::timer_service;
using stepwright::testing::manual_timer_service;

namespace {
struct logged_entry : timer_service::entry {
    std::vector<int>* log = nullptr;
    int id = 0;
    timer_service* service = nullptr;
    timer_service::clock::time_point seen_now;
    logged_entry() { fire = &on_fire; }
    static void on_fire(timer_service::entry* e) noexcept {
        auto& self = static_cast<logged_entry&>(*e);
        if (self.service) {
            self.seen_now = self.service->now();
        }
        self.log->push_back(self.id);
    }
};
struct counting_entry : timer_service::entry {
    std::atomic<int> fired{0};
    std::atomic<bool> running{false};
    int spin = 0;
    counting_entry() { fire = &on_fire; }
    static void on_fire(timer_service::entry* e) noexcept {
        auto& self = static_cast<counting_entry&>(*e);
        self.running.store(true);
        for (int i = 0; i < self.spin; ++i) {
            std::this_thread::yield();
        }
        self.fired.fetch_add(1);
        self.running.store(false);
    }
};
} // namespace

TEST(manual_timer, fires_in_deadline_order_with_fifo_ties) {
    manual_timer_service timers;
    std::vector<int> log;
    std::vector<logged_entry> entries(6);
    const auto base = timers.now();
    const std::array<int, 6> delays{30, 10, 20, 10, 30, 10};
    for (int i = 0; i < 6; ++i) {
        auto& e = entries[static_cast<std::size_t>(i)];
        e.log = &log;
        e.id = i;
        e.service = &timers;
        timers.schedule(e, base + std::chrono::milliseconds(delays[static_cast<std::size_t>(i)]));
    }
    EXPECT_EQ(timers.pending(), 6U);
    timers.advance(15ms);
    EXPECT_EQ(log, (std::vector<int>{1, 3, 5}));
    EXPECT_EQ(entries[1].seen_now, base + 10ms);
    EXPECT_EQ(timers.now(), base + 15ms);
    timers.advance_to(base + 100ms);
    EXPECT_EQ(log, (std::vector<int>{1, 3, 5, 2, 0, 4}));
    EXPECT_EQ(timers.pending(), 0U);
    timers.advance_to(base); // the past: no-op
    EXPECT_EQ(timers.now(), base + 100ms);
}

TEST(manual_timer, cancel_before_and_after_fire) {
    manual_timer_service timers;
    std::vector<int> log;
    logged_entry a;
    logged_entry b;
    a.log = &log;
    b.log = &log;
    a.id = 1;
    b.id = 2;
    timers.schedule(a, timers.now() + 5ms);
    timers.schedule(b, timers.now() + 5ms);
    EXPECT_TRUE(timers.cancel(a));
    EXPECT_FALSE(timers.cancel(a));
    timers.advance(5ms);
    EXPECT_EQ(log, (std::vector<int>{2}));
    EXPECT_FALSE(timers.cancel(b));
    // Rescheduling after cancel or fire is allowed.
    timers.schedule(a, timers.now());
    timers.advance(0ms);
    EXPECT_EQ(log, (std::vector<int>{2, 1}));
}

TEST(manual_timer, reschedule_and_cancel_inside_callback) {
    manual_timer_service timers;
    struct periodic : timer_service::entry {
        manual_timer_service* timers = nullptr;
        int count = 0;
        bool cancel_result = true;
        periodic() {
            fire = [](timer_service::entry* e) noexcept {
                auto& self = static_cast<periodic&>(*e);
                self.cancel_result = self.timers->cancel(self); // own callback: false
                if (++self.count < 5) {
                    self.timers->schedule(self, self.timers->now() + 10ms);
                }
            };
        }
    } p;
    p.timers = &timers;
    timers.schedule(p, timers.now() + 10ms);
    timers.advance(45ms); // 10, 20, 30, 40
    EXPECT_EQ(p.count, 4);
    EXPECT_FALSE(p.cancel_result);
    timers.advance(100ms);
    EXPECT_EQ(p.count, 5);
    EXPECT_EQ(timers.pending(), 0U);
}

TEST(manual_timer, heap_stress_random_schedule_and_cancel) {
    manual_timer_service timers;
    std::mt19937 random(12345);
    constexpr int count = 2000;
    std::vector<int> log;
    std::vector<logged_entry> entries(count);
    std::vector<bool> cancelled(count, false);
    std::vector<int> deadline(count);
    for (int i = 0; i < count; ++i) {
        auto& e = entries[static_cast<std::size_t>(i)];
        e.log = &log;
        e.id = i;
        deadline[static_cast<std::size_t>(i)] = static_cast<int>(random() % 500);
        timers.schedule(e, timers.now() +
                               std::chrono::milliseconds(deadline[static_cast<std::size_t>(i)]));
        if (i % 3 == 0) {
            const auto victim = static_cast<std::size_t>(random() % static_cast<unsigned>(i + 1));
            if (!cancelled[victim]) {
                EXPECT_TRUE(timers.cancel(entries[victim]));
                cancelled[victim] = true;
            }
        }
        if (i % 500 == 0) {
            timers.advance(0ms);
        }
    }
    timers.advance(1s);
    std::size_t expected = 0;
    for (bool c : cancelled) {
        expected += c ? 0 : 1;
    }
    ASSERT_EQ(log.size(), expected);
    for (std::size_t i = 1; i < log.size(); ++i) {
        const auto prev = static_cast<std::size_t>(log[i - 1]);
        const auto next = static_cast<std::size_t>(log[i]);
        EXPECT_TRUE(deadline[prev] < deadline[next] ||
                    (deadline[prev] == deadline[next] && prev < next))
            << i;
    }
}

TEST(manual_timer, regression_cancel_waits_for_callback_on_other_thread) {
    manual_timer_service timers;
    counting_entry e;
    e.spin = 2000;
    timers.schedule(e, timers.now());
    std::thread firing([&] { timers.advance(0ms); });
    while (!e.running.load() && e.fired.load() == 0) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(timers.cancel(e));
    EXPECT_EQ(e.fired.load(), 1); // cancel returned only after the callback finished
    firing.join();
}

TEST(steady_timer, fires_in_order_with_tolerance) {
    stepwright::steady_timer_service timers;
    std::vector<int> log;
    std::atomic<int> fired{0};
    struct entry_t : timer_service::entry {
        std::vector<int>* log = nullptr;
        std::atomic<int>* fired = nullptr;
        int id = 0;
        entry_t() {
            fire = [](timer_service::entry* e) noexcept {
                auto& self = static_cast<entry_t&>(*e);
                self.log->push_back(self.id);
                self.fired->fetch_add(1, std::memory_order_release);
            };
        }
    };
    std::vector<entry_t> entries(3);
    const auto start = timers.now();
    const std::array<int, 3> delays{30, 10, 20};
    for (int i = 0; i < 3; ++i) {
        auto& e = entries[static_cast<std::size_t>(i)];
        e.log = &log;
        e.fired = &fired;
        e.id = i;
        timers.schedule(e, start + std::chrono::milliseconds(delays[static_cast<std::size_t>(i)]));
    }
    while (fired.load(std::memory_order_acquire) != 3) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(log, (std::vector<int>{1, 2, 0}));
    EXPECT_GE(timers.now() - start, 30ms);
    EXPECT_LT(timers.now() - start, 5s);
}

TEST(steady_timer, cancel_before_fire_prevents_callback) {
    stepwright::steady_timer_service timers;
    counting_entry e;
    timers.schedule(e, timers.now() + 1h);
    EXPECT_TRUE(timers.cancel(e));
    EXPECT_EQ(e.fired.load(), 0);
    timers.schedule(e, timers.now() + 1ms);
    while (e.fired.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(timers.cancel(e));
}

TEST(steady_timer, regression_cancel_races_with_firing) {
    stepwright::steady_timer_service timers;
    const int iterations = test_support::stress_iterations(2000);
    counting_entry e;
    e.spin = 10;
    int removed = 0;
    for (int i = 0; i < iterations; ++i) {
        const int before = e.fired.load();
        timers.schedule(e, timers.now() + std::chrono::microseconds(i % 50));
        for (int spin = 0; spin < i % 64; ++spin) {
            std::this_thread::yield();
        }
        if (timers.cancel(e)) {
            ++removed;
            EXPECT_EQ(e.fired.load(), before);
        } else {
            // Not removed: the callback has fully run by the time cancel returns.
            EXPECT_EQ(e.fired.load(), before + 1);
            EXPECT_FALSE(e.running.load());
        }
    }
    std::printf("removed %d of %d\n", removed, iterations);
}

TEST(steady_timer, destruction_stops_thread_promptly) {
    for (int i = 0; i < 10; ++i) {
        const auto start = std::chrono::steady_clock::now();
        {
            stepwright::steady_timer_service timers;
            counting_entry e;
            timers.schedule(e, timers.now() + 1h);
            EXPECT_TRUE(timers.cancel(e));
        }
        EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
    }
}

TEST(steady_timer, regression_destroy_with_scheduled_entry_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            counting_entry e;
            stepwright::steady_timer_service timers;
            timers.schedule(e, timers.now() + 1h);
        },
        "destroying a steady_timer_service with scheduled timers");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
