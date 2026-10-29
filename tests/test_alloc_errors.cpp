// Allocation behaviour of error paths: library control-flow codes share immortal
// payloads, every other payload whose message fits a block is recycled through
// per-thread magazines and a shared depot, and longer messages allocate exactly
// once. Under AddressSanitizer blocks are not recycled (so use-after-free stays
// detectable): the recycled-block cases then check behaviour only, while cases
// that create nothing but library codes keep their zero-allocation assertions.
#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using stepwright::errc;
using stepwright::error;

namespace {
constexpr int iterations = 10'000;
constexpr bool recycling = stepwright::detail::error_recycling;
constexpr std::string_view message40 = "query 17 failed: connection reset by peer";
static_assert(message40.size() >= 36 && message40.size() <= 44);
constexpr std::array<errc, 4> library_codes{errc::cancelled, errc::timed_out, errc::closed,
                                            errc::broken_promise};

// Zero after warm-up wherever blocks are recycled; under ASan only behaviour counts.
void expect_steady_state(std::size_t made) {
    if constexpr (recycling) {
        EXPECT_EQ(made, 0U);
    } else {
        static_cast<void>(made);
    }
}
// Warm up once, then count `iterations` runs. run returns true when its outcome
// was as expected; returns the allocation count and checks every outcome.
template <class F>
std::size_t allocations_after_warm_up(F&& run) {
    EXPECT_TRUE(run());
    int failed = 0;
    const auto made = count_allocations([&] {
        for (int i = 0; i < iterations; ++i) {
            failed += run() ? 0 : 1;
        }
    });
    EXPECT_EQ(failed, 0);
    return made;
}

stepwright::detail::error_block_cache& thread_cache() noexcept {
    return stepwright::detail::error_blocks::local();
}

// Completes only when cancelled, with a shared, pre-built error value (as an
// I/O driver reporting through a value it already holds): no error of its own.
struct query_op : stepwright::task {
    stepwright::completion receiver;
    stepwright::cancel_registration registration;
    void on_start(stepwright::completion done, stepwright::cancel_token token) override {
        receiver = done;
        registration.arm(
            token,
            [](void* context) noexcept {
                auto& self = *static_cast<query_op*>(context);
                self.registration.disarm();
                const auto deliver = self.receiver;
                deliver(stepwright::detail::cancelled_error());
            },
            this);
    }
};

// Parks; default cancellation wakes it with errc::cancelled, which it reports.
struct cancelled_leaf : stepwright::machine<cancelled_leaf> {
    step begin() { return park<&cancelled_leaf::woken>(); }
    step woken(const error& e) { return e.is(errc::cancelled) ? fail(errc::cancelled) : done(); }
};
// Fails with its child's error after adding its own frame (and its type name).
template <class Child>
struct failing_parent : stepwright::machine<failing_parent<Child>> {
    using base = stepwright::machine<failing_parent<Child>>;
    using typename base::step;
    Child child;
    step begin() { return this->template after<&failing_parent::failed>(child); }
    step failed(const error& e) {
        error annotated = e;
        annotated.add_context("failing_parent::failed");
        return this->fail(std::move(annotated));
    }
};
template <class Child>
struct forwarding : stepwright::machine<forwarding<Child>> {
    using base = stepwright::machine<forwarding<Child>>;
    using typename base::step;
    Child child;
    step begin() { return this->forward(child); }
};
struct failing_op : stepwright::machine<failing_op> {
    step begin() { return fail(std::errc::connection_reset, message40); }
};

// Releases its error during this thread's exit, after the cache's exit hook.
std::atomic<int> late_release_state{-1};
struct late_holder {
    error held;
    late_holder() = default;
    late_holder(const late_holder&) = delete;
    late_holder& operator=(const late_holder&) = delete;
    ~late_holder() {
        const auto& cache = thread_cache();
        const bool untouched = cache.loaded.rounds == 0 && cache.previous.rounds == 0;
        held = {};
        const bool still_untouched = cache.loaded.rounds == 0 && cache.previous.rounds == 0;
        late_release_state.store(untouched && still_untouched ? cache.state : -2);
    }
};
late_holder& thread_holder() {
    thread_local late_holder holder;
    return holder;
}

// Destroyed during static destruction, after the main thread's thread-local
// destructors: its release must not touch the dead cache.
struct static_holder {
    error held;
    static_holder() = default;
    static_holder(const static_holder&) = delete;
    static_holder& operator=(const static_holder&) = delete;
    ~static_holder() {
        if (!held) {
            return;
        }
        const auto& cache = thread_cache();
        const bool dead = cache.state == stepwright::detail::error_block_cache::dead;
        held = {};
        if (dead && (cache.loaded.rounds != 0 || cache.previous.rounds != 0)) {
            std::fputs("error released into a dead thread cache\n", stderr);
            std::_Exit(1);
        }
    }
};
static_holder static_late;
} // namespace

// Case 1. Must stay first: no warm-up is needed before the first library code.
TEST(alloc_errors, library_codes_never_allocate) {
    for (const auto code : library_codes) {
        int failed = 0;
        EXPECT_EQ(count_allocations([&] {
                      for (int i = 0; i < iterations; ++i) {
                          error made = error::make(code);
                          error copy = made; // NOLINT(performance-unnecessary-copy-initialization)
                          error moved = std::move(copy);
                          copy = moved;
                          moved = std::move(copy);
                          error implicit = error::make(stepwright::make_error_code(code));
                          failed +=
                              moved.is(code) && implicit.is(code) && made.message().empty() ? 0 : 1;
                      }
                  }),
                  0U);
        EXPECT_EQ(failed, 0);
    }
    // One shared payload per code, also reached through make(error_code) and fail().
    EXPECT_EQ(error::make(errc::closed).context().data(),
              error::make(std::error_code(errc::closed)).context().data());
    EXPECT_NE(error::make(errc::closed).context().data(),
              error::make(errc::timed_out).context().data());
}

// The downstream reproduction: with_deadline used to allocate one payload per
// timeout (error::make(errc::timed_out)); 10,000 timeouts made 10,000 allocations.
TEST(alloc_errors, regression_with_deadline_timeouts_allocate_nothing) {
    stepwright::testing::manual_timer_service timers;
    query_op query;
    stepwright::with_deadline guarded(query, timers, 5ms);
    EXPECT_EQ(allocations_after_warm_up([&] {
                  test_support::receiver receiver;
                  guarded.start(receiver.callback());
                  timers.advance(5ms);
                  return receiver.calls == 1 && receiver.result.is(errc::timed_out);
              }),
              0U);
}

// Case 2. The timer wins against a cooperative task and against failing machines.
TEST(alloc_errors, with_deadline_timer_wins) {
    stepwright::testing::manual_timer_service timers;
    test_support::cancellable_child cooperative;
    stepwright::with_deadline task_deadline(cooperative, timers, 5ms);
    // The cooperative child reports errc::cancelled: library codes only.
    EXPECT_EQ(allocations_after_warm_up([&] {
                  test_support::receiver receiver;
                  task_deadline.start(receiver.callback());
                  timers.advance(5ms);
                  return receiver.result.is(errc::timed_out);
              }),
              0U);

    failing_parent<cancelled_leaf> one;
    failing_parent<failing_parent<failing_parent<cancelled_leaf>>> three;
    for (stepwright::task* inner :
         {static_cast<stepwright::task*>(&one), static_cast<stepwright::task*>(&three)}) {
        stepwright::with_deadline deadline(*inner, timers, 5ms);
        expect_steady_state(allocations_after_warm_up([&] {
            test_support::receiver receiver;
            deadline.start(receiver.callback());
            timers.advance(5ms);
            return receiver.result.is(errc::timed_out) && receiver.result.context().empty();
        }));
    }
}

// Case 3. Default cancellation of a park, through fail and two forwarding machines.
TEST(alloc_errors, default_cancellation_through_forwarding_machines) {
    forwarding<forwarding<test_support::parked>> root;
    stepwright::cancel_source source;
    expect_steady_state(allocations_after_warm_up([&] {
        test_support::receiver receiver;
        root.start(receiver.callback(), source.token());
        source.request_cancel();
        stepwright::detail::cancel_source_access::reset(source);
        return receiver.result.is(errc::cancelled) && receiver.result.context().size() == 3;
    }));
}

// Case 4. A child fails with a ~40-character message; its siblings are cancelled.
TEST(alloc_errors, joins_cancel_siblings_on_error) {
    test_support::cancellable_child first;
    test_support::cancellable_child second;
    failing_op failing;
    stepwright::when_all fixed(first, second, failing);
    std::array<stepwright::task*, 3> children{&first, &second, &failing};
    stepwright::when_all_range range(children);
    for (stepwright::task* join :
         {static_cast<stepwright::task*>(&fixed), static_cast<stepwright::task*>(&range)}) {
        expect_steady_state(allocations_after_warm_up([&] {
            test_support::receiver receiver;
            join->start(receiver.callback());
            return receiver.calls == 1 && receiver.result.message() == message40 &&
                   receiver.result.context().size() == 1 &&
                   first.cancelled.load() == second.cancelled.load();
        }));
    }
    EXPECT_EQ(first.cancelled.load(), 2 * (iterations + 1));
}

// Case 5. closed to a waiting consumer and to waiting senders, broken_promise
// through a shared_task, and retry observing timed_out.
TEST(alloc_errors, channel_close_shared_task_and_retry) {
    // Channels cannot reopen: build one pair per run before counting.
    std::deque<stepwright::channel<int>> channels;
    for (int i = 0; i < 2 * (iterations + 1); ++i) {
        channels.emplace_back(1);
    }
    std::size_t next = 0;
    expect_steady_state(allocations_after_warm_up([&] {
        auto& consumed = channels[next++];
        auto& sent = channels[next++];
        test_support::receiver ready;
        consumed.on_ready(ready.callback(), {});
        consumed.close(error::make(errc::closed));
        const auto pulled = consumed.try_pull();
        bool ok =
            ready.calls == 1 && pulled.is_failed() && pulled.failure_reason().is(errc::closed);
        ok = ok && sent.try_send(0);
        stepwright::channel<int>::send_op one(sent, 1);
        stepwright::channel<int>::send_op two(sent, 2);
        test_support::receiver r1;
        test_support::receiver r2;
        one.start(r1.callback());
        two.start(r2.callback());
        sent.close();
        return ok && r1.result.is(errc::closed) && r2.result.is(errc::closed);
    }));
    channels.clear();

    test_support::async_child producer;
    stepwright::shared_task shared(producer);
    auto w1 = shared.wait();
    auto w2 = shared.wait();
    // Library codes only.
    EXPECT_EQ(allocations_after_warm_up([&] {
                  test_support::receiver a;
                  test_support::receiver b;
                  w1.start(a.callback());
                  w2.start(b.callback());
                  producer.finish(error::make(errc::broken_promise));
                  shared.reset();
                  return a.result.is(errc::broken_promise) && b.result.is(errc::broken_promise);
              }),
              0U);

    stepwright::testing::virtual_executor ex;
    auto attempt =
        stepwright::make_task([](stepwright::completion done, const stepwright::cancel_token&) {
            done(error::make(errc::timed_out));
        });
    stepwright::retry retry(attempt, ex, {.max_attempts = 3, .initial_backoff = 1ms});
    expect_steady_state(allocations_after_warm_up([&] {
        const error result = ex.run(retry);
        return result.is(errc::timed_out) && retry.attempts() == 3 && result.context().size() == 1;
    }));
}

// Case 6. Errors made on one thread and released on another reach a steady state:
// the releasing thread's full magazines flow back through the depot.
TEST(alloc_errors, cross_thread_release_reaches_steady_state) {
    constexpr int batch = 250;
    // 250 is not a multiple of the magazine size, so the magazine fill levels cycle
    // with a period of 16 rounds; warm up over several periods.
    constexpr int warm_up_rounds = 64;
    constexpr int rounds = 400; // 100,000 errors
    std::array<error, batch> slots;
    std::atomic<int> published{0};
    std::atomic<int> drained{0};
    std::atomic<int> bad{0};
    // Atomics only: libc++'s std::barrier is not TSan-instrumented on macOS.
    std::thread consumer([&] {
        for (int round = 1; round <= warm_up_rounds + rounds; ++round) {
            while (published.load(std::memory_order_acquire) != round) {
                std::this_thread::yield();
            }
            for (auto& e : slots) {
                if (!e) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
                e = {};
            }
            drained.store(round, std::memory_order_release);
        }
    });
    const auto produce = [&](int round) {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            switch (i % 3) {
                case 0:
                    slots[i] = error::make(std::errc::connection_reset, message40);
                    break;
                case 1:
                    slots[i] = error::make(std::errc::io_error);
                    break;
                default:
                    slots[i] = error::make(errc::timed_out);
                    slots[i].add_context("producer");
                    break;
            }
        }
        published.store(round, std::memory_order_release);
        while (drained.load(std::memory_order_acquire) != round) {
            std::this_thread::yield();
        }
    };
    for (int round = 1; round <= warm_up_rounds; ++round) {
        produce(round);
    }
    expect_steady_state(count_allocations([&] {
        for (int round = warm_up_rounds + 1; round <= warm_up_rounds + rounds; ++round) {
            produce(round);
        }
    }));
    consumer.join();
    EXPECT_EQ(bad.load(), 0);
}

// Case 7. A thread that cached blocks exits: they go to the depot and are reused;
// errors it made outlive it; a thread-local destroyed after its cache bypasses it.
TEST(alloc_errors, thread_exit_returns_blocks_and_late_releases_bypass_cache) {
    constexpr std::size_t burst = 1000;
    std::vector<error> made(burst);
    std::vector<error> survivors(8);
    std::thread worker([&] {
        thread_holder(); // constructed before the cache's exit hook: destroyed after it
        for (auto& e : made) {
            e = error::make(std::errc::io_error, message40);
        }
        for (auto& e : made) {
            e = {};
        }
        for (auto& e : survivors) {
            e = error::make(errc::closed, message40);
        }
        thread_holder().held = error::make(std::errc::io_error, message40);
    });
    worker.join();
    if constexpr (recycling) {
        EXPECT_EQ(late_release_state.load(), stepwright::detail::error_block_cache::dead);
        EXPECT_GE(stepwright::detail::error_block_depot::instance().size(),
                  burst - (2 * stepwright::detail::error_magazine_size));
    }
    for (const auto& e : survivors) {
        EXPECT_TRUE(e.is(errc::closed));
        EXPECT_EQ(e.message(), message40);
    }
    survivors.clear();
    // The exited thread's blocks serve this thread without allocating.
    std::vector<error> reused(burst - 100);
    expect_steady_state(count_allocations([&] {
        for (auto& e : reused) {
            e = error::make(std::errc::io_error, message40);
        }
    }));
    reused.clear();
    // Released during static destruction, after this thread's cache hook.
    static_late.held = error::make(std::errc::io_error, message40);
}

// Case 8. A message longer than a block allocates exactly once and frees.
TEST(alloc_errors, large_message_allocates_once) {
    const std::string large(stepwright::detail::error_message_capacity + 1, 'x');
    const std::string fits(stepwright::detail::error_message_capacity, 'y');
    error e;
    EXPECT_EQ(count_allocations([&] { e = error::make(std::errc::io_error, large); }), 1U);
    EXPECT_EQ(e.message(), large);
    error copy = e;
    EXPECT_EQ(count_allocations([&] { copy.add_context("clone"); }), 1U); // copy-on-write
    EXPECT_EQ(copy.message(), large);
    EXPECT_TRUE(e.context().empty());
    EXPECT_EQ(count_allocations([&] {
                  e = {};
                  copy = {};
              }),
              0U);
    // The longest message that fits a block is recycled.
    expect_steady_state(allocations_after_warm_up([&] {
        const error boundary = error::make(std::errc::io_error, fits);
        return boundary.message() == fits;
    }));
}

// Case 9. Observable behaviour is the same for immortal, recycled and large payloads.
TEST(alloc_errors, semantics_are_unchanged_for_every_storage) {
    const std::string large(stepwright::detail::error_message_capacity + 10, 'z');
    struct sample {
        error made;
        std::string head;
    };
    const std::array<sample, 4> samples{
        sample{.made = error::make(errc::timed_out), .head = "timed_out"},
        sample{.made = error::make(std::errc::io_error),
               .head = std::make_error_code(std::errc::io_error).message()},
        sample{.made = error::make(errc::closed, "drained"), .head = "closed: drained"},
        sample{.made = error::make(errc::broken_promise, large),
               .head = "broken_promise: " + large}};
    for (const auto& [made, head] : samples) {
        EXPECT_EQ(made.describe(), head);
        EXPECT_TRUE(made.context().empty());
        EXPECT_EQ(made.dropped_context(), 0U);
        error copy = made;
        copy.add_context("first");
        EXPECT_EQ(copy.describe(), head + " [first]");
        EXPECT_EQ(made.describe(), head); // copy-on-write, also for immortal payloads
        for (int i = 0; i < 10; ++i) {
            copy.add_context("more");
        }
        EXPECT_EQ(copy.context().size(), 8U);
        EXPECT_EQ(copy.dropped_context(), 3U);
        EXPECT_STREQ(copy.context()[0], "first");
        EXPECT_EQ(copy.code(), made.code());
        EXPECT_EQ(copy.message(), made.message());
        EXPECT_TRUE(made.context().empty());
        EXPECT_EQ(made.dropped_context(), 0U);
    }
    // An immortal payload is never written: a fresh handle still has no context.
    EXPECT_TRUE(error::make(errc::timed_out).context().empty());
    EXPECT_EQ(error::make(errc::timed_out).dropped_context(), 0U);
    // A unique recycled payload is extended in place; a reused block starts clean.
    for (int round = 0; round < 3; ++round) {
        error e = error::make(std::errc::io_error, "disk");
        EXPECT_TRUE(e.context().empty());
        EXPECT_EQ(e.dropped_context(), 0U);
        const auto* frames = e.context().data();
        for (int i = 0; i < 12; ++i) {
            e.add_context("frame");
        }
        EXPECT_EQ(e.context().data(), frames);
        EXPECT_EQ(e.context().size(), 8U);
        EXPECT_EQ(e.dropped_context(), 4U);
    }
    // Code-only errors of other categories are not shared.
    const error a = error::make(std::errc::io_error);
    const error b = error::make(std::errc::io_error);
    EXPECT_NE(a.context().data(), b.context().data());
    EXPECT_EQ(a.code(), b.code());
    // Out-of-range stepwright values are ordinary payloads.
    const error unknown = error::make(std::error_code(99, stepwright::category()));
    EXPECT_EQ(unknown.describe(), "unknown stepwright error");
    EXPECT_FALSE(unknown.is(errc::cancelled));
}

// Free blocks are bounded: a thread keeps two magazines, the depot at most its
// limit, and magazines beyond that are freed.
TEST(alloc_errors, free_blocks_are_bounded) {
    constexpr std::size_t limit = stepwright::detail::error_depot_limit;
    constexpr std::size_t magazine = stepwright::detail::error_magazine_size;
    std::vector<error> held(limit + 1000);
    for (auto& e : held) {
        e = error::make(std::errc::io_error, message40);
    }
    held.clear();
    if constexpr (recycling) {
        const auto& depot = stepwright::detail::error_block_depot::instance();
        EXPECT_LE(depot.size(), limit);
        EXPECT_GT(depot.size(), limit - magazine);
        EXPECT_LE(thread_cache().loaded.rounds + thread_cache().previous.rounds, 2 * magazine);
    }
}
