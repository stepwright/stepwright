#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
// An item that counts its runs and optionally posts a child item from the worker.
struct node : stepwright::work_item {
    stepwright::thread_pool* pool = nullptr;
    node* child = nullptr;
    std::atomic<int> runs{0};
    std::atomic<bool> on_pool{false};
    node() noexcept { run = &execute; }
    static void execute(stepwright::work_item* item) noexcept {
        auto& self = *static_cast<node*>(item);
        self.runs.fetch_add(1, std::memory_order_relaxed);
        self.on_pool.store(self.pool->running_in_this_thread(), std::memory_order_relaxed);
        if (self.child != nullptr) {
            self.pool->post(*self.child);
        }
    }
};

// Posts itself again until `remaining` reaches zero (a chain on one worker,
// possibly stolen by others).
struct chain : stepwright::work_item {
    stepwright::thread_pool* pool = nullptr;
    long remaining = 0;
    std::atomic<long>* total = nullptr;
    chain() noexcept {
        run = [](stepwright::work_item* item) noexcept {
            auto& self = *static_cast<chain*>(item);
            self.total->fetch_add(1, std::memory_order_relaxed);
            if (--self.remaining > 0) {
                self.pool->post(self);
            }
        };
    }
};

std::chrono::microseconds process_cpu_time() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto to_us = [](const timeval& t) {
        return std::chrono::seconds(t.tv_sec) + std::chrono::microseconds(t.tv_usec);
    };
    return to_us(usage.ru_utime) + to_us(usage.ru_stime);
}
} // namespace

TEST(thread_pool, size_and_default_construction) {
    stepwright::thread_pool one(0);
    EXPECT_EQ(one.size(), 1U);
    stepwright::thread_pool four(4);
    EXPECT_EQ(four.size(), 4U);
    stepwright::thread_pool hardware;
    EXPECT_GE(hardware.size(), 1U);
}

TEST(thread_pool, every_item_runs_exactly_once) {
    // 500,000 items posted from four outside threads, each posting one more item
    // from the worker that runs it: 1,000,000 items in total.
    constexpr std::size_t outer = 500'000;
    stepwright::thread_pool pool(8);
    std::vector<node> outside(outer);
    std::vector<node> nested(outer);
    for (std::size_t i = 0; i < outer; ++i) {
        outside[i].pool = &pool;
        outside[i].child = &nested[i];
        nested[i].pool = &pool;
    }
    std::vector<std::thread> posters;
    constexpr std::size_t poster_count = 4;
    posters.reserve(poster_count);
    for (std::size_t t = 0; t < poster_count; ++t) {
        posters.emplace_back([&, t] {
            for (std::size_t i = t; i < outer; i += poster_count) {
                pool.post(outside[i]);
            }
        });
    }
    for (auto& poster : posters) {
        poster.join();
    }
    pool.wait_idle();
    std::size_t bad = 0;
    for (std::size_t i = 0; i < outer; ++i) {
        bad += outside[i].runs.load() != 1 || nested[i].runs.load() != 1 ||
               !outside[i].on_pool.load() || !nested[i].on_pool.load();
    }
    EXPECT_EQ(bad, 0U);
}

TEST(thread_pool, running_in_this_thread) {
    stepwright::thread_pool pool(2);
    stepwright::thread_pool other(1);
    EXPECT_FALSE(pool.running_in_this_thread());
    std::atomic<int> seen{0};
    pool.post([&] {
        seen.fetch_add(pool.running_in_this_thread() ? 1 : 100);
        seen.fetch_add(other.running_in_this_thread() ? 100 : 1);
    });
    other.post([&] { seen.fetch_add(pool.running_in_this_thread() ? 100 : 1); });
    pool.wait_idle();
    other.wait_idle();
    EXPECT_EQ(seen.load(), 3);
}

TEST(thread_pool, wait_idle_waits_for_nested_work) {
    stepwright::thread_pool pool(4);
    pool.wait_idle(); // idle pool: returns at once
    std::atomic<long> total{0};
    std::array<chain, 8> chains;
    for (auto& c : chains) {
        c.pool = &pool;
        c.remaining = 20'000;
        c.total = &total;
    }
    for (auto& c : chains) {
        pool.post(c);
    }
    pool.wait_idle();
    EXPECT_EQ(total.load(), 8 * 20'000);
    // Again after the workers went to sleep.
    std::this_thread::sleep_for(20ms);
    for (auto& c : chains) {
        c.remaining = 1'000;
        pool.post(c);
    }
    pool.wait_idle();
    EXPECT_EQ(total.load(), 8 * 21'000);
}

TEST(thread_pool, regression_wait_idle_from_pool_thread_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::thread_pool pool(1);
            pool.post([&] { pool.wait_idle(); });
            std::this_thread::sleep_for(10s);
        },
        "wait_idle from a thread_pool thread");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}

TEST(thread_pool, regression_post_after_shutdown_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::thread_pool pool(1);
            pool.shutdown();
            node n;
            n.pool = &pool;
            pool.post(n);
        },
        "post to a thread_pool after shutdown started");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}

TEST(thread_pool, regression_wait_idle_counts_stolen_items) {
    // A worker fans out items that the other workers steal. wait_idle() used to
    // count a post only after publishing the item, so a thief could run it and
    // count its completion first; wait_idle() could then return early and the next
    // round re-posted items that were still queued (corrupting the queue).
    const int rounds = test_support::stress_iterations(100);
    constexpr std::size_t fan = 10'000;
    stepwright::thread_pool pool(8);
    std::vector<node> items(fan);
    for (std::size_t i = 0; i < fan; ++i) {
        items[i].pool = &pool;
    }
    struct spawner : stepwright::work_item {
        node* items = nullptr;
        stepwright::thread_pool* pool = nullptr;
    } source;
    source.items = items.data();
    source.pool = &pool;
    source.run = [](stepwright::work_item* item) noexcept {
        auto& self = *static_cast<spawner*>(item);
        for (std::size_t i = 0; i < fan; ++i) {
            self.pool->post(self.items[i]);
        }
    };
    for (int round = 1; round <= rounds; ++round) {
        pool.post(source);
        pool.wait_idle();
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < fan; ++i) {
            wrong += items[i].runs.load(std::memory_order_relaxed) != round;
        }
        std::size_t over = 0;
        for (std::size_t i = 0; i < fan; ++i) {
            over += items[i].runs.load(std::memory_order_relaxed) > round;
        }
        ASSERT_EQ(wrong, 0U) << "round " << round << " over " << over;
    }
}

TEST(thread_pool, shutdown_runs_pending_and_nested_items) {
    const long rounds = test_support::stress_iterations(5);
    for (long round = 0; round < rounds; ++round) {
        constexpr long outer = 10'000;
        std::atomic<long> total{0};
        auto pool = std::make_unique<stepwright::thread_pool>(4);
        // Each outer item starts a chain of three: outer -> nested -> nested.
        std::vector<chain> chains(outer);
        for (std::size_t i = 0; i < static_cast<std::size_t>(outer); ++i) {
            chains[i].pool = pool.get();
            chains[i].remaining = 3;
            chains[i].total = &total;
            pool->post(chains[i]);
        }
        if (round % 2 == 0) {
            pool->shutdown();
            EXPECT_EQ(total.load(), 3 * outer);
            pool->shutdown(); // idempotent
            pool.reset();
        } else {
            pool.reset(); // the destructor shuts down
        }
        EXPECT_EQ(total.load(), 3 * outer);
    }
}

TEST(thread_pool, concurrent_shutdown_calls) {
    std::atomic<long> total{0};
    stepwright::thread_pool pool(4);
    std::array<chain, 4> chains;
    for (auto& c : chains) {
        c.pool = &pool;
        c.remaining = 50'000;
        c.total = &total;
        pool.post(c);
    }
    std::thread first([&] { pool.shutdown(); });
    std::thread second([&] { pool.shutdown(); });
    pool.shutdown();
    first.join();
    second.join();
    EXPECT_EQ(total.load(), 4 * 50'000);
}

TEST(thread_pool, stress_eight_posting_threads) {
    const long per_thread = test_support::stress_iterations(20'000);
    constexpr int posters = 8;
    stepwright::thread_pool pool(4);
    std::atomic<long> total{0};
    std::vector<std::vector<chain>> items;
    items.reserve(posters);
    for (int t = 0; t < posters; ++t) {
        items.emplace_back(static_cast<std::size_t>(per_thread));
    }
    const auto count = static_cast<std::size_t>(per_thread);
    test_support::race_rounds rounds([&] {
        std::vector<std::function<void()>> actions;
        actions.reserve(posters);
        for (int t = 0; t < posters; ++t) {
            actions.emplace_back([&, t] {
                auto& mine = items[static_cast<std::size_t>(t)];
                for (std::size_t i = 0; i < count; ++i) {
                    mine[i].pool = &pool;
                    mine[i].remaining = 1 + static_cast<long>(i % 3); // some re-post
                    mine[i].total = &total;
                    pool.post(mine[i]);
                    if (i % 1024 == 0) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        return actions;
    }());
    rounds.run_round();
    pool.wait_idle();
    long expected = 0;
    for (long i = 0; i < per_thread; ++i) {
        expected += 1 + (i % 3);
    }
    EXPECT_EQ(total.load(), expected * posters);
}

TEST(thread_pool, idle_workers_sleep_and_wake_promptly) {
    stepwright::thread_pool pool(8);
    std::atomic<long> total{0};
    std::array<chain, 16> chains;
    for (auto& c : chains) {
        c.pool = &pool;
        c.remaining = 1'000;
        c.total = &total;
        pool.post(c);
    }
    pool.wait_idle();
    std::this_thread::sleep_for(50ms); // let the searchers give up and sleep
    const auto before = process_cpu_time();
    const auto wall_before = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(200ms);
    const auto cpu = process_cpu_time() - before;
    const auto wall = std::chrono::steady_clock::now() - wall_before;
    std::cout << "idle cpu=" << cpu.count() << "us over "
              << std::chrono::duration_cast<std::chrono::microseconds>(wall).count()
              << "us wall with 8 workers\n";
    // Eight spinning workers would burn ~1.6 s of CPU here.
    EXPECT_LT(cpu, 100ms);

    // Wake-up latency from full sleep.
    for (int i = 0; i < 5; ++i) {
        std::atomic<bool> ran{false};
        const auto posted = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point started;
        pool.post([&] {
            started = std::chrono::steady_clock::now();
            ran.store(true, std::memory_order_release);
        });
        while (!ran.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto latency = started - posted;
        std::cout << "wake latency="
                  << std::chrono::duration_cast<std::chrono::microseconds>(latency).count()
                  << "us\n";
        EXPECT_LT(latency, 1s);
        std::this_thread::sleep_for(20ms);
    }
}

namespace {
struct hopper : stepwright::machine<hopper> {
    stepwright::thread_pool* pool = nullptr;
    test_support::async_child* child = nullptr;
    std::atomic<int>* off_pool = nullptr;
    int steps = 0;
    step begin() { return after<&hopper::on_child>(*child); }
    step on_child() {
        if (!pool->running_in_this_thread()) {
            off_pool->fetch_add(1);
        }
        ++steps;
        return defer<&hopper::on_deferred>();
    }
    step on_deferred() {
        if (!pool->running_in_this_thread()) {
            off_pool->fetch_add(1);
        }
        ++steps;
        return done();
    }
};
} // namespace

TEST(thread_pool, bound_machines_continue_on_the_pool) {
    constexpr int count = 1'000;
    stepwright::thread_pool pool(4);
    std::atomic<int> off_pool{0};
    std::vector<test_support::async_child> children(count);
    std::vector<hopper> machines(count);
    std::vector<test_support::receiver> receivers(count);
    for (int i = 0; i < count; ++i) {
        auto& m = machines[static_cast<std::size_t>(i)];
        m.pool = &pool;
        m.child = &children[static_cast<std::size_t>(i)];
        m.off_pool = &off_pool;
        m.bind(pool);
        m.start(receivers[static_cast<std::size_t>(i)].callback());
    }
    // Completions arrive from a foreign thread.
    std::thread completer([&] {
        for (auto& child : children) {
            child.finish();
        }
    });
    completer.join();
    for (auto& r : receivers) {
        r.wait();
    }
    pool.wait_idle();
    EXPECT_EQ(off_pool.load(), 0);
    for (const auto& m : machines) {
        EXPECT_EQ(m.steps, 2);
    }
}
