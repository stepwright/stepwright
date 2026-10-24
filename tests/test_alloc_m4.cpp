#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace {
// Re-posts itself until `remaining` reaches zero: posts from a worker.
struct chain : stepwright::work_item {
    stepwright::executor* ex = nullptr;
    long remaining = 0;
    std::atomic<long>* total = nullptr;
    chain() noexcept {
        run = [](stepwright::work_item* item) noexcept {
            auto& self = *static_cast<chain*>(item);
            self.total->fetch_add(1, std::memory_order_relaxed);
            if (--self.remaining > 0) {
                self.ex->post(self);
            }
        };
    }
};

// A machine bound to an executor hopping through it (child completions and
// deferrals).
struct hopper : stepwright::machine<hopper> {
    explicit hopper(stepwright::executor& ex) : hop_(ex) { bind(ex); }
    int remaining = 0;
    step begin() {
        if (remaining == 0) {
            return done();
        }
        --remaining;
        return after<&hopper::hopped>(hop_);
    }
    step hopped() { return defer<&hopper::begin>(); }

private:
    stepwright::reschedule hop_;
};

// Make sure every worker thread is running: a sanitizer runtime may allocate when
// it registers a thread, and under load a worker can start after the warm-up run.
// Each item blocks until all of them run, so each occupies its own worker (a
// stuck single item is stolen).
void start_all_workers(stepwright::thread_pool& pool) {
    std::atomic<std::size_t> started{0};
    const std::size_t n = pool.size();
    for (std::size_t i = 0; i < n; ++i) {
        pool.post([&started, n] {
            started.fetch_add(1);
            while (started.load() < n) {
                std::this_thread::yield();
            }
        });
    }
    pool.wait_idle();
}

// Run fn once to warm up, then require that a second run allocates nothing.
template <class F>
void expect_steady_state_zero_alloc(F&& fn, long expected) {
    EXPECT_EQ(fn(), expected);
    long result = 0;
    EXPECT_EQ(count_allocations([&] { result = fn(); }), 0U);
    EXPECT_EQ(result, expected);
}
} // namespace

TEST(alloc_m4, regression_zero_alloc_thread_pool_posts) {
    stepwright::thread_pool pool(4);
    start_all_workers(pool);
    std::array<chain, 64> chains;
    std::atomic<long> total{0};
    expect_steady_state_zero_alloc(
        [&] {
            total = 0;
            for (auto& c : chains) {
                c.ex = &pool;
                c.remaining = 100; // one outside post, 99 posts from workers
                c.total = &total;
                pool.post(c);
            }
            pool.wait_idle();
            return total.load();
        },
        6400);
}

TEST(alloc_m4, regression_zero_alloc_machines_bound_to_pool) {
    stepwright::thread_pool pool(4);
    start_all_workers(pool);
    std::array<std::optional<hopper>, 16> machines;
    for (auto& m : machines) {
        m.emplace(pool);
    }
    std::array<test_support::receiver, 16> receivers;
    expect_steady_state_zero_alloc(
        [&] {
            long completed = 0;
            for (std::size_t i = 0; i < machines.size(); ++i) {
                receivers[i].completed.store(false);
                machines[i]->remaining = 50;
                machines[i]->start(receivers[i].callback());
            }
            for (auto& r : receivers) {
                r.wait();
                ++completed;
            }
            pool.wait_idle();
            return completed;
        },
        16);
}

TEST(alloc_m4, regression_zero_alloc_virtual_executor) {
    stepwright::testing::virtual_executor fifo;
    stepwright::testing::virtual_executor random(stepwright::testing::schedule::random, 42);
    for (stepwright::testing::virtual_executor* ex : {&fifo, &random}) {
        std::array<chain, 8> chains;
        std::atomic<long> total{0};
        hopper machine(*ex);
        stepwright::delay nap(*ex, 1ms);
        expect_steady_state_zero_alloc(
            [&] {
                total = 0;
                for (auto& c : chains) {
                    c.ex = ex;
                    c.remaining = 10;
                    c.total = &total;
                    ex->post(c);
                }
                machine.remaining = 10;
                test_support::receiver done;
                machine.start(done.callback());
                test_support::receiver slept;
                nap.start(slept.callback());
                ex->run_until_quiescent();
                return total.load() + done.calls + slept.calls;
            },
            82);
    }
}

TEST(alloc_m4, regression_cancelled_error_allocates_once) {
    const stepwright::error& first = stepwright::detail::cancelled_error();
    EXPECT_EQ(count_allocations([&] {
                  for (int i = 0; i < 100; ++i) {
                      // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): under test
                      const stepwright::error copy = stepwright::detail::cancelled_error();
                      EXPECT_EQ(&stepwright::detail::cancelled_error(), &first);
                      EXPECT_TRUE(copy.is(stepwright::errc::cancelled));
                  }
              }),
              0U);
}
