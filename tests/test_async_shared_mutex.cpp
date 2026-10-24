#include <gtest/gtest.h>

#include "test_support.hpp"
#include <barrier>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using stepwright::errc;
using stepwright::error;

TEST(async_shared_mutex, readers_are_concurrent_writer_is_exclusive) {
    stepwright::async_shared_mutex m;
    auto r1 = m.lock_shared();
    auto r2 = m.lock_shared();
    test_support::receiver a;
    test_support::receiver b;
    r1.start(a.callback());
    r2.start(b.callback());
    EXPECT_EQ(a.calls + b.calls, 2);
    EXPECT_FALSE(m.try_lock());
    EXPECT_TRUE(m.try_lock_shared());
    m.unlock_shared();
    auto w = m.lock();
    test_support::receiver writer;
    w.start(writer.callback());
    EXPECT_EQ(writer.calls, 0);
    m.unlock_shared();
    EXPECT_EQ(writer.calls, 0);
    m.unlock_shared();
    EXPECT_EQ(writer.calls, 1);
    EXPECT_FALSE(m.try_lock_shared());
    EXPECT_FALSE(m.try_lock());
    m.unlock();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_shared_mutex, writer_preference_and_reader_batch_admission) {
    stepwright::async_shared_mutex m;
    ASSERT_TRUE(m.try_lock_shared());
    std::vector<std::string> order;
    struct recorder {
        std::vector<std::string>* order;
        std::string name;
        void done(const error&) noexcept { order->push_back(name); }
    };
    std::vector<recorder> recorders{{.order = &order, .name = "w1"},
                                    {.order = &order, .name = "r1"},
                                    {.order = &order, .name = "r2"},
                                    {.order = &order, .name = "w2"},
                                    {.order = &order, .name = "r3"}};
    auto w1 = m.lock();
    auto r1 = m.lock_shared();
    auto r2 = m.lock_shared();
    auto w2 = m.lock();
    auto r3 = m.lock_shared();
    w1.start(stepwright::completion::bind<&recorder::done>(recorders.data()));
    // A writer is queued: new readers queue behind it even though readers hold it.
    EXPECT_FALSE(m.try_lock_shared());
    r1.start(stepwright::completion::bind<&recorder::done>(&recorders[1]));
    r2.start(stepwright::completion::bind<&recorder::done>(&recorders[2]));
    w2.start(stepwright::completion::bind<&recorder::done>(&recorders[3]));
    r3.start(stepwright::completion::bind<&recorder::done>(&recorders[4]));
    EXPECT_TRUE(order.empty());
    m.unlock_shared();
    EXPECT_EQ(order, (std::vector<std::string>{"w1"}));
    m.unlock(); // w1 releases: consecutive readers r1, r2 admitted together
    EXPECT_EQ(order, (std::vector<std::string>{"w1", "r1", "r2"}));
    m.unlock_shared();
    EXPECT_EQ(order.size(), 3U);
    m.unlock_shared(); // last reader: w2
    EXPECT_EQ(order.back(), "w2");
    m.unlock();
    EXPECT_EQ(order.back(), "r3");
    m.unlock_shared();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_shared_mutex, cancelled_writer_releases_blocked_readers) {
    stepwright::async_shared_mutex m;
    ASSERT_TRUE(m.try_lock_shared());
    stepwright::cancel_source source;
    auto w = m.lock();
    auto r = m.lock_shared();
    test_support::receiver writer;
    test_support::receiver reader;
    w.start(writer.callback(), source.token());
    r.start(reader.callback());
    EXPECT_EQ(reader.calls, 0);
    source.request_cancel();
    EXPECT_EQ(writer.calls, 1);
    EXPECT_TRUE(writer.result.is(errc::cancelled));
    EXPECT_EQ(reader.calls, 1); // admitted once the writer left the queue
    EXPECT_FALSE(reader.result);
    m.unlock_shared();
    m.unlock_shared();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_shared_mutex, cancelled_reader_and_cancel_after_grant) {
    stepwright::async_shared_mutex m;
    ASSERT_TRUE(m.try_lock());
    stepwright::cancel_source cancel_reader;
    stepwright::cancel_source late;
    auto r1 = m.lock_shared();
    auto r2 = m.lock_shared();
    test_support::receiver a;
    test_support::receiver b;
    r1.start(a.callback(), cancel_reader.token());
    r2.start(b.callback(), late.token());
    cancel_reader.request_cancel();
    EXPECT_TRUE(a.result.is(errc::cancelled));
    m.unlock();
    EXPECT_EQ(b.calls, 1);
    late.request_cancel(); // after the grant: no effect
    EXPECT_FALSE(b.result);
    m.unlock_shared();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_shared_mutex, guarded_variants) {
    stepwright::async_shared_mutex m;
    test_support::sync_child inner;
    inner.result = error::make(errc::closed);
    stepwright::async_shared_mutex::guarded exclusive(m, inner);
    stepwright::async_shared_mutex::guarded_shared shared(m, inner);
    test_support::receiver a;
    test_support::receiver b;
    exclusive.start(a.callback());
    shared.start(b.callback());
    EXPECT_TRUE(a.result.is(errc::closed));
    EXPECT_TRUE(b.result.is(errc::closed));
    EXPECT_TRUE(m.try_lock());
    m.unlock();
    // Queued guarded_shared behind a writer, then cancelled.
    ASSERT_TRUE(m.try_lock());
    stepwright::cancel_source source;
    test_support::receiver c;
    shared.start(c.callback(), source.token());
    source.request_cancel();
    EXPECT_TRUE(c.result.is(errc::cancelled));
    EXPECT_EQ(inner.calls, 2);
    m.unlock();
}

TEST(async_shared_mutex, regression_stress_exclusivity_invariants) {
    constexpr int threads = 8;
    constexpr int machines_per_thread = 4;
    const int iterations = test_support::stress_iterations(400);
    stepwright::async_shared_mutex m;
    std::atomic<int> readers{0};
    std::atomic<int> writers{0};
    std::atomic<int> violations{0};
    std::atomic<int> max_readers{0};
    struct section : stepwright::task {
        bool exclusive = false;
        std::atomic<int>* readers = nullptr;
        std::atomic<int>* writers = nullptr;
        std::atomic<int>* violations = nullptr;
        std::atomic<int>* max_readers = nullptr;
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            if (exclusive) {
                if (writers->fetch_add(1) != 0 || readers->load() != 0) {
                    violations->fetch_add(1);
                }
                std::this_thread::yield();
                writers->fetch_sub(1);
            } else {
                const int now = readers->fetch_add(1) + 1;
                if (writers->load() != 0) {
                    violations->fetch_add(1);
                }
                int seen = max_readers->load();
                while (now > seen && !max_readers->compare_exchange_weak(seen, now)) {
                }
                readers->fetch_sub(1);
            }
            done({});
        }
    };
    struct worker : stepwright::machine<worker> {
        worker(stepwright::async_shared_mutex& m, section& w, section& r)
            : exclusive(m, w)
            , shared(m, r) {}
        stepwright::async_shared_mutex::guarded exclusive;
        stepwright::async_shared_mutex::guarded_shared shared;
        int remaining = 0;
        unsigned seed = 0;
        step begin() {
            if (remaining-- == 0) {
                return done();
            }
            seed = seed * 1664525U + 1013904223U;
            if ((seed >> 16) % 4 == 0) {
                return after<&worker::begin>(exclusive);
            }
            return after<&worker::begin>(shared);
        }
    };
    section write_section;
    section read_section;
    for (auto* s : {&write_section, &read_section}) {
        s->readers = &readers;
        s->writers = &writers;
        s->violations = &violations;
        s->max_readers = &max_readers;
    }
    write_section.exclusive = true;
    std::vector<std::unique_ptr<worker>> workers;
    for (int i = 0; i < threads * machines_per_thread; ++i) {
        workers.push_back(std::make_unique<worker>(m, write_section, read_section));
        workers.back()->remaining = iterations;
        workers.back()->seed = static_cast<unsigned>(i) * 7919U;
    }
    std::atomic<int> finished{0};
    const auto count_done = stepwright::completion{
        .fn =
            [](void* ctx, const error& e) noexcept {
                EXPECT_FALSE(e);
                static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_release);
            },
        .ctx = &finished};
    std::barrier sync(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < machines_per_thread; ++i) {
                workers[(static_cast<std::size_t>(t) * machines_per_thread) +
                        static_cast<std::size_t>(i)]
                    ->start(count_done);
            }
        });
    }
    for (auto& thread : pool) {
        thread.join();
    }
    while (finished.load(std::memory_order_acquire) != threads * machines_per_thread) {
        std::this_thread::yield();
    }
    EXPECT_EQ(violations.load(), 0);
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(async_shared_mutex, regression_cancel_vs_grant_race) {
    const int iterations = test_support::stress_iterations(20'000);
    stepwright::async_shared_mutex m;
    stepwright::cancel_source* source = nullptr;
    test_support::race_rounds rounds({[&] { m.unlock(); }, [&] { source->request_cancel(); }});
    auto reader = m.lock_shared();
    auto writer = m.lock();
    int granted = 0;
    for (int i = 0; i < iterations; ++i) {
        const bool exclusive = i % 2 == 1;
        ASSERT_TRUE(m.try_lock());
        stepwright::cancel_source local;
        source = &local;
        test_support::receiver receiver;
        if (exclusive) {
            writer.start(receiver.callback(), local.token());
        } else {
            reader.start(receiver.callback(), local.token());
        }
        rounds.run_round();
        EXPECT_EQ(receiver.calls, 1);
        if (!receiver.result) {
            ++granted;
            if (exclusive) {
                m.unlock();
            } else {
                m.unlock_shared();
            }
        } else {
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
        }
        ASSERT_TRUE(m.try_lock());
        m.unlock();
    }
    std::printf("granted %d of %d\n", granted, iterations);
}

TEST(async_shared_mutex, regression_destroy_held_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::async_shared_mutex m;
            m.try_lock_shared();
        },
        "destroying a held async_shared_mutex");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
