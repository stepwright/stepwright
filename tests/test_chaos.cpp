// Permanent cross-primitive stress test (spec M3 §0.2, based on the M2 review's
// chaos probe). Four threads repeatedly run
//   when_all( with_deadline( async_mutex::guarded(critical), timers, random ) x 4 )
// on a steady timer service with random timeouts and random external
// cancellation. Inside the mutex, `critical` checks exclusion, sends a tagged value
// to a shared bounded channel (waiting for space, cancellable) and holds the lock
// for a random delay. A consumer machine drains merge(channel, interval) on
// whatever thread delivers readiness. Checks: mutual exclusion, exactly-once
// completion, no hang, results in {ok, timed_out, cancelled}, and that the consumer
// received exactly the successfully sent values. The second test runs the same
// units with every machine bound to a thread_pool (spec M4 §5).
#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
constexpr std::uint64_t tag = std::uint64_t{1} << 63;

struct world {
    stepwright::steady_timer_service* timers = nullptr;
    stepwright::channel<std::uint64_t>* channel = nullptr;
    stepwright::async_mutex mutex;
    std::atomic<int> inside{0};
    std::atomic<bool> violated{false};
    std::atomic<long> sections{0};
    long shared = 0; // protected by mutex only
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<long> sent{0};
    std::atomic<std::uint64_t> sent_sum{0};
    std::atomic<long> send_failures{0};
};

struct critical_op : stepwright::machine<critical_op> {
    world* w = nullptr;
    std::chrono::microseconds hold{};
    std::uint64_t value = 0;
    std::optional<stepwright::channel<std::uint64_t>::send_op> send;
    std::optional<stepwright::delay> wait;

    step begin() {
        if (w->inside.fetch_add(1) != 0) {
            w->violated.store(true);
        }
        ++w->shared;
        w->sections.fetch_add(1);
        value = tag | w->sequence.fetch_add(1);
        send.emplace(*w->channel, value);
        return after<&critical_op::sent>(*send);
    }
    step sent(const stepwright::error& e) {
        if (!e) {
            w->sent.fetch_add(1);
            w->sent_sum.fetch_add(value);
        } else if (!e.is(stepwright::errc::cancelled)) {
            w->send_failures.fetch_add(1);
        }
        wait.emplace(*w->timers, hold);
        return after<&critical_op::slept>(*wait);
    }
    step slept(const stepwright::error& e) {
        w->inside.fetch_sub(1);
        return fail(e);
    }
};

struct unit {
    unit(world& w, stepwright::thread_pool* pool) : guard(w.mutex, op) {
        op.w = &w;
        if (pool != nullptr) {
            op.bind(*pool);
            guard.bind(*pool);
        }
    }
    critical_op op;
    stepwright::async_mutex::guarded guard;
    std::optional<stepwright::with_deadline> deadline;
};

struct sink {
    std::atomic<int> done{0};
    stepwright::error result;
};

void run_chaos(bool on_pool) {
    const int rounds = test_support::stress_iterations(1000);
    stepwright::steady_timer_service timers;
    std::optional<stepwright::thread_pool> pool_storage;
    stepwright::thread_pool* pool = nullptr;
    if (on_pool) {
        pool = &pool_storage.emplace(4);
    }
    stepwright::channel<std::uint64_t> channel(2);
    world w;
    w.timers = &timers;
    w.channel = &channel;

    // Consumer: merge(channel, interval) drained on the delivering thread.
    auto ticks = stepwright::interval(timers, 200us);
    stepwright::merge merged(channel, ticks);
    std::atomic<long> received{0};
    std::atomic<std::uint64_t> received_sum{0};
    std::atomic<long> tick_items{0};
    auto consumer = stepwright::for_each(merged, [&](std::uint64_t v) {
        if ((v & tag) != 0) {
            received_sum.fetch_add(v);
            received.fetch_add(1);
        } else {
            tick_items.fetch_add(1);
        }
    });
    if (pool != nullptr) {
        consumer.bind(*pool);
    }
    stepwright::cancel_source consumer_cancel;
    test_support::receiver consumer_done;
    consumer.start(consumer_done.callback(), consumer_cancel.token());

    std::atomic<long> starts{0};
    std::atomic<long> completions{0};
    std::atomic<long> ok{0};
    std::atomic<long> timed_out{0};
    std::atomic<long> cancelled{0};
    std::atomic<long> other{0};
    std::atomic<bool> hung{false};
    std::atomic<bool> duplicate{false};

    auto worker = [&](unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> hold_us(0, 150);
        std::uniform_int_distribution<int> timeout_us(0, 400);
        std::uniform_int_distribution<int> coin(0, 3);
        for (int r = 0; r < rounds && !hung.load(); ++r) {
            std::array<unit, 4> units{unit(w, pool), unit(w, pool), unit(w, pool), unit(w, pool)};
            for (auto& u : units) {
                u.op.hold = std::chrono::microseconds(hold_us(rng));
                u.deadline.emplace(u.guard, timers, std::chrono::microseconds(timeout_us(rng)));
            }
            stepwright::when_all all(*units[0].deadline, *units[1].deadline, *units[2].deadline,
                                     *units[3].deadline);
            stepwright::cancel_source source;
            sink out;
            starts.fetch_add(1);
            all.start({.fn =
                           [](void* ctx, const stepwright::error& e) noexcept {
                               auto* s = static_cast<sink*>(ctx);
                               s->result = e;
                               s->done.fetch_add(1, std::memory_order_release);
                           },
                       .ctx = &out},
                      source.token());
            if (coin(rng) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(hold_us(rng)));
                source.request_cancel();
            }
            const auto limit = std::chrono::steady_clock::now() + 10s;
            while (out.done.load(std::memory_order_acquire) == 0) {
                if (std::chrono::steady_clock::now() > limit) {
                    hung.store(true);
                    std::cerr << "chaos: hang detected\n";
                    std::abort(); // the operation is still running: cannot unwind
                }
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(10us);
            if (out.done.load() != 1) {
                duplicate.store(true);
            }
            completions.fetch_add(1);
            if (!out.result) {
                ok.fetch_add(1);
            } else if (out.result.is(stepwright::errc::timed_out)) {
                timed_out.fetch_add(1);
            } else if (out.result.is(stepwright::errc::cancelled)) {
                cancelled.fetch_add(1);
            } else {
                other.fetch_add(1);
            }
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back(worker, 1234 + t);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Every successfully sent value reaches the consumer.
    const auto limit = std::chrono::steady_clock::now() + 10s;
    while (received.load() != w.sent.load() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(100us);
    }
    consumer_cancel.request_cancel();
    consumer_done.wait();

    std::cout << "rounds=" << starts.load() << " ok=" << ok.load()
              << " timed_out=" << timed_out.load() << " cancelled=" << cancelled.load()
              << " sections=" << w.sections.load() << " sent=" << w.sent.load()
              << " received=" << received.load() << " ticks=" << tick_items.load() << '\n';
    EXPECT_FALSE(w.violated.load());
    EXPECT_FALSE(duplicate.load());
    EXPECT_EQ(starts.load(), completions.load());
    EXPECT_EQ(w.sections.load(), w.shared);
    EXPECT_EQ(other.load(), 0);
    EXPECT_EQ(w.send_failures.load(), 0);
    EXPECT_EQ(received.load(), w.sent.load());
    EXPECT_EQ(received_sum.load(), w.sent_sum.load());
    EXPECT_TRUE(consumer_done.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(channel.size(), 0U);
    if (pool != nullptr) {
        pool->wait_idle();
    }
}
} // namespace

TEST(chaos, regression_primitives_and_streams_under_threads) {
    run_chaos(false);
}

TEST(chaos, primitives_and_streams_on_a_thread_pool) {
    run_chaos(true);
}
