#include <gtest/gtest.h>

#include "test_support.hpp"
#include <atomic>
#include <barrier>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using stepwright::errc;
using stepwright::error;

TEST(when_all, zero_children_complete_synchronously) {
    stepwright::when_all<0> all(std::array<stepwright::task*, 0>{});
    stepwright::cancel_source source;
    source.request_cancel();
    test_support::receiver receiver;
    all.start(receiver.callback(), source.token());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
    stepwright::when_all_range range(std::span<stepwright::task* const>{});
    test_support::receiver range_receiver;
    range.start(range_receiver.callback());
    EXPECT_EQ(range_receiver.calls, 1);
}

TEST(when_all, one_and_many_synchronous_children) {
    test_support::sync_child a;
    test_support::sync_child b;
    test_support::sync_child c;
    stepwright::when_all one(a);
    test_support::receiver r1;
    one.start(r1.callback());
    EXPECT_EQ(r1.calls, 1);
    stepwright::when_all all(a, b, c);
    static_assert(std::is_same_v<decltype(all), stepwright::when_all<3>>);
    test_support::receiver r3;
    all.start(r3.callback());
    EXPECT_EQ(r3.calls, 1);
    EXPECT_FALSE(r3.result);
    EXPECT_EQ(a.calls, 2);
    EXPECT_EQ(c.calls, 1);
}

TEST(when_all, mixed_sync_and_async_children_wait_for_all) {
    test_support::sync_child a;
    test_support::async_child b;
    test_support::sync_child c;
    test_support::async_child d;
    stepwright::when_all all(a, b, c, d);
    test_support::receiver receiver;
    all.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    d.finish();
    EXPECT_EQ(receiver.calls, 0);
    b.finish();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
}

TEST(when_all, first_error_wins_and_cancels_siblings) {
    test_support::async_child first;
    test_support::cancellable_child sibling;
    test_support::async_child second;
    stepwright::when_all all(first, sibling, second);
    test_support::receiver receiver;
    all.start(receiver.callback());
    second.finish(error::make(errc::closed, "second"));
    EXPECT_EQ(sibling.cancelled.load(), 1);
    EXPECT_TRUE(first.received_token.cancel_requested());
    first.finish(error::make(errc::timed_out, "first"));
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::closed));
    EXPECT_EQ(receiver.result.message(), "second");
}

TEST(when_all, run_all_does_not_cancel_siblings) {
    test_support::sync_child failing;
    failing.result = error::make(errc::closed);
    test_support::cancellable_child sibling;
    stepwright::when_all all(std::array<stepwright::task*, 2>{&failing, &sibling},
                             stepwright::when_all_policy::run_all);
    test_support::receiver receiver;
    all.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    EXPECT_EQ(sibling.cancelled.load(), 0);
    EXPECT_FALSE(sibling.received_token.can_be_cancelled());
    sibling.complete();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::closed));
}

TEST(when_all, parent_cancellation) {
    test_support::cancellable_child a;
    test_support::cancellable_child b;
    stepwright::when_all all(a, b);
    stepwright::cancel_source source;
    test_support::receiver receiver;
    all.start(receiver.callback(), source.token());
    source.request_cancel();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_EQ(a.cancelled.load() + b.cancelled.load(), 2);
    // Children that ignore cancellation and succeed still report cancelled.
    test_support::async_child c;
    test_support::async_child d;
    stepwright::when_all ignoring(c, d);
    stepwright::cancel_source second;
    test_support::receiver r2;
    ignoring.start(r2.callback(), second.token());
    second.request_cancel();
    c.finish();
    d.finish();
    EXPECT_TRUE(r2.result.is(errc::cancelled));
    // A child failing for another reason wins over cancellation.
    stepwright::cancel_source third;
    test_support::receiver r3;
    ignoring.start(r3.callback(), third.token());
    third.request_cancel();
    c.finish(error::make(errc::closed));
    d.finish();
    EXPECT_TRUE(r3.result.is(errc::closed));
}

TEST(when_all, pre_cancelled_parent_reaches_children) {
    test_support::cancellable_child a;
    test_support::sync_child b;
    stepwright::when_all all(a, b);
    stepwright::cancel_source source;
    source.request_cancel();
    test_support::receiver receiver;
    all.start(receiver.callback(), source.token());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::cancelled));
    EXPECT_EQ(a.cancelled.load(), 1);
}

TEST(when_all, structured_completion_waits_for_late_child) {
    test_support::sync_child failing;
    failing.result = error::make(errc::closed);
    test_support::async_child late; // ignores cancellation
    stepwright::when_all all(late, failing);
    test_support::receiver receiver;
    all.start(receiver.callback());
    EXPECT_TRUE(late.received_token.cancel_requested());
    EXPECT_EQ(receiver.calls, 0);
    late.finish();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(errc::closed));
}

TEST(when_all, restart_after_cancelled_run) {
    test_support::sync_child failing;
    test_support::cancellable_child sibling;
    stepwright::when_all all(sibling, failing);
    for (int round = 0; round < 4; ++round) {
        failing.result = round % 2 == 0 ? error::make(errc::closed) : error{};
        test_support::receiver receiver;
        all.start(receiver.callback());
        if (round % 2 == 0) {
            EXPECT_EQ(receiver.calls, 1); // sibling was cancelled by the failure
            EXPECT_TRUE(receiver.result.is(errc::closed));
        } else {
            EXPECT_FALSE(sibling.received_token.cancel_requested());
            sibling.complete();
            EXPECT_EQ(receiver.calls, 1);
            EXPECT_FALSE(receiver.result);
        }
    }
}

TEST(when_all, range_of_ten_thousand_children) {
    constexpr std::size_t count = 10'000;
    std::vector<test_support::sync_child> sync(count / 2);
    std::vector<test_support::async_child> async(count / 2);
    std::vector<stepwright::task*> children;
    children.reserve(count);
    for (std::size_t i = 0; i < count / 2; ++i) {
        children.push_back(&sync[i]);
        children.push_back(&async[i]);
    }
    stepwright::when_all_range all(children);
    for (int round = 0; round < 2; ++round) {
        test_support::receiver receiver;
        all.start(receiver.callback());
        for (auto& child : async) {
            EXPECT_EQ(receiver.calls, 0);
            child.finish();
        }
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_FALSE(receiver.result);
    }
}

TEST(when_all, regression_children_complete_on_many_threads) {
    constexpr std::size_t count = 64;
    const int rounds = test_support::stress_iterations(200);
    std::vector<test_support::async_child> children(count);
    std::vector<stepwright::task*> pointers;
    pointers.reserve(children.size());
    for (auto& child : children) {
        pointers.push_back(&child);
    }
    stepwright::when_all_range all(pointers);
    stepwright::cancel_source parent;
    for (int round = 0; round < rounds; ++round) {
        test_support::receiver receiver;
        all.start(receiver.callback(), parent.token());
        std::barrier sync(8);
        std::vector<std::thread> threads;
        threads.reserve(8);
        for (std::size_t t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                sync.arrive_and_wait();
                for (std::size_t i = t; i < count; i += 8) {
                    children[i].finish(i == 17 && round % 3 == 0 ? error::make(errc::closed)
                                                                 : error{});
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_EQ(receiver.result.is(errc::closed), round % 3 == 0);
    }
}

TEST(when_all, regression_parent_cancel_vs_completion_race) {
    const int iterations = test_support::stress_iterations(20'000);
    test_support::cancellable_child a;
    test_support::cancellable_child b;
    stepwright::when_all all(a, b);
    stepwright::cancel_source* source = nullptr;
    test_support::race_rounds rounds({[&] { source->request_cancel(); },
                                      [&] {
                                          a.complete();
                                          b.complete();
                                      }});
    int cancelled = 0;
    for (int i = 0; i < iterations; ++i) {
        stepwright::cancel_source local;
        source = &local;
        test_support::receiver receiver;
        all.start(receiver.callback(), local.token());
        rounds.run_round();
        EXPECT_EQ(receiver.calls, 1);
        if (receiver.result) {
            EXPECT_TRUE(receiver.result.is(errc::cancelled));
            ++cancelled;
        }
    }
    std::printf("cancelled %d of %d\n", cancelled, iterations);
}

// M5: a when_all_range of 64 or more children counts completions in shards.
// Children bound to a pool complete on many threads; the first failure, the
// cancelled parent and success are all reported, never before the last child.
TEST(when_all, regression_sharded_completion_on_a_pool) {
    struct leaf : stepwright::machine<leaf> {
        std::atomic<int>* finished = nullptr;
        bool failing = false;
        step begin() { return defer<&leaf::finish_up>(); }
        step finish_up() {
            finished->fetch_add(1, std::memory_order_relaxed);
            return failing ? fail(error::make(errc::closed)) : done();
        }
    };
    for (const std::size_t count : {std::size_t{64}, std::size_t{65}, std::size_t{1000}}) {
        stepwright::thread_pool pool(8);
        std::atomic<int> finished{0};
        std::vector<leaf> leaves(count);
        std::vector<stepwright::task*> children;
        for (auto& l : leaves) {
            l.bind(pool);
            l.finished = &finished;
            children.push_back(&l);
        }
        for (const auto policy :
             {stepwright::when_all_policy::cancel_on_error, stepwright::when_all_policy::run_all}) {
            stepwright::when_all_range all(children, policy);
            for (int round = 0; round < test_support::stress_iterations(30); ++round) {
                // Rounds cycle through: no failure, failures in different shards,
                // and a parent cancelled while the children run.
                const auto failing = static_cast<std::size_t>(round * 37) % (count + 1);
                for (std::size_t i = 0; i < count; ++i) {
                    leaves[i].failing = i == failing || (round % 4 == 1 && i == count - 1);
                }
                const bool cancel_parent = round % 4 == 3;
                stepwright::cancel_source parent;
                std::atomic<int> seen_at_completion{-1};
                struct probe {
                    std::atomic<int>* finished;
                    std::atomic<int>* seen;
                    test_support::receiver* receiver;
                };
                test_support::receiver receiver;
                probe ctx{
                    .finished = &finished, .seen = &seen_at_completion, .receiver = &receiver};
                finished.store(0, std::memory_order_relaxed);
                all.start({.fn =
                               [](void* p, const error& e) noexcept {
                                   auto& self = *static_cast<probe*>(p);
                                   self.seen->store(self.finished->load(std::memory_order_relaxed),
                                                    std::memory_order_relaxed);
                                   self.receiver->finish(e);
                               },
                           .ctx = &ctx},
                          parent.token());
                if (cancel_parent) {
                    parent.request_cancel();
                }
                receiver.wait();
                EXPECT_EQ(receiver.calls, 1);
                EXPECT_EQ(seen_at_completion.load(), static_cast<int>(count));
                const bool any_failure = failing < count || round % 4 == 1;
                if (any_failure) {
                    EXPECT_TRUE(receiver.result.is(errc::closed)) << count << " " << round;
                } else if (cancel_parent) {
                    // Cancelled unless every child had already completed.
                    EXPECT_TRUE(!receiver.result || receiver.result.is(errc::cancelled));
                } else {
                    EXPECT_FALSE(receiver.result) << count << " " << round;
                }
                pool.wait_idle();
            }
        }
    }
}
