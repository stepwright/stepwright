#include <gtest/gtest.h>

#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using stepwright::testing::schedule;
using stepwright::testing::virtual_executor;

namespace {
using time_point = stepwright::timer_service::clock::time_point;
constexpr time_point origin{};

// A posted item that records its id and may post follow-ups.
struct recorder : stepwright::work_item {
    virtual_executor* ex = nullptr;
    std::vector<int>* trace = nullptr;
    int id = 0;
    std::vector<recorder*> follow_ups;
    recorder() noexcept {
        run = [](stepwright::work_item* item) noexcept {
            auto& self = *static_cast<recorder*>(item);
            self.trace->push_back(self.id);
            for (auto* follow_up : self.follow_ups) {
                self.ex->post(*follow_up);
            }
        };
    }
};

// A timer entry that records its id when it fires.
struct timer_probe : stepwright::timer_service::entry {
    std::vector<int>* trace = nullptr;
    int id = 0;
    timer_probe() noexcept {
        fire = [](stepwright::timer_service::entry* e) noexcept {
            auto& self = static_cast<timer_probe&>(*e);
            self.trace->push_back(self.id);
        };
    }
};

// 20 items, every third posting two follow-ups (14), and 6 timers due now: 40.
std::vector<int> run_program(schedule policy, std::uint64_t seed) {
    virtual_executor ex(policy, seed);
    std::vector<int> trace;
    std::vector<recorder> items(60);
    for (std::size_t i = 0; i < items.size(); ++i) {
        items[i].ex = &ex;
        items[i].trace = &trace;
        items[i].id = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < 20; ++i) {
        if (i % 3 == 0) {
            items[i].follow_ups = {&items[20 + (2 * i)], &items[21 + (2 * i)]};
        }
        ex.post(items[i]);
    }
    std::array<timer_probe, 6> timers;
    for (std::size_t i = 0; i < timers.size(); ++i) {
        timers[i].trace = &trace;
        timers[i].id = 100 + static_cast<int>(i);
        ex.schedule(timers[i], ex.now());
    }
    ex.run_until_idle();
    return trace;
}
} // namespace

TEST(virtual_executor, fifo_order) {
    const auto trace = run_program(schedule::fifo, 0);
    // Posting order: items 0..19 and the six timers made due after them, then the
    // follow-ups in the order they were posted.
    std::vector<int> expected;
    expected.reserve(40);
    for (int i = 0; i < 20; ++i) {
        expected.push_back(i);
    }
    for (int i = 0; i < 6; ++i) {
        expected.push_back(100 + i);
    }
    for (int i = 0; i < 20; i += 3) {
        expected.push_back(20 + (2 * i));
        expected.push_back(21 + (2 * i));
    }
    EXPECT_EQ(trace, expected);
}

TEST(virtual_executor, fifo_interleaves_timers_by_the_moment_they_became_due) {
    virtual_executor ex;
    std::vector<int> trace;
    recorder a;
    recorder b;
    a.ex = b.ex = &ex;
    a.trace = b.trace = &trace;
    a.id = 1;
    b.id = 3;
    timer_probe t;
    t.trace = &trace;
    t.id = 2;
    ex.post(a);
    ex.schedule(t, origin); // due now, after a
    ex.post(b);
    EXPECT_EQ(ex.pending(), 3U);
    EXPECT_EQ(ex.run_until_idle(), 3U);
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3}));
}

TEST(virtual_executor, seeded_random_order_is_reproducible_and_seed_dependent) {
    std::set<std::vector<int>> distinct;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const auto first = run_program(schedule::random, seed);
        const auto second = run_program(schedule::random, seed);
        EXPECT_EQ(first, second) << "seed " << seed;
        EXPECT_EQ(first.size(), 40U) << "seed " << seed;
        distinct.insert(first);
    }
    EXPECT_GE(distinct.size(), 15U);
    EXPECT_NE(run_program(schedule::random, 1), run_program(schedule::fifo, 1));
}

TEST(virtual_executor, delay_moves_virtual_time) {
    virtual_executor ex;
    stepwright::delay wait(ex, 5s);
    test_support::receiver done;
    wait.start(done.callback());
    EXPECT_EQ(ex.scheduled_timers(), 1U);
    EXPECT_EQ(ex.run_until_idle(), 0U); // time does not move
    ex.run_for(4s);
    EXPECT_EQ(done.calls, 0);
    EXPECT_EQ(ex.now() - origin, 4s);
    EXPECT_EQ(ex.run_for(1s), 1U);
    EXPECT_EQ(done.calls, 1);
    EXPECT_FALSE(done.result);
    EXPECT_EQ(ex.now() - origin, 5s);
    // Cancellation of a pending virtual delay.
    stepwright::cancel_source source;
    test_support::receiver cancelled;
    wait.start(cancelled.callback(), source.token());
    source.request_cancel();
    EXPECT_TRUE(cancelled.result.is(stepwright::errc::cancelled));
    EXPECT_EQ(ex.scheduled_timers(), 0U);
    EXPECT_EQ(ex.run_until_quiescent(), 0U);
}

TEST(virtual_executor, with_deadline_on_virtual_time) {
    virtual_executor ex;
    stepwright::delay slow(ex, 10s);
    stepwright::with_deadline timeout(slow, ex, 3s);
    test_support::receiver done;
    timeout.start(done.callback());
    ex.run_until_quiescent();
    EXPECT_TRUE(done.result.is(stepwright::errc::timed_out));
    EXPECT_EQ(ex.now() - origin, 3s); // the inner delay was cancelled, not waited for

    stepwright::delay fast(ex, 1s);
    stepwright::with_deadline in_time(fast, ex, 3s);
    test_support::receiver ok;
    in_time.start(ok.callback());
    ex.run_until_quiescent();
    EXPECT_EQ(ok.calls, 1);
    EXPECT_FALSE(ok.result);
    EXPECT_EQ(ex.now() - origin, 4s);
    EXPECT_EQ(ex.scheduled_timers(), 0U);
}

TEST(virtual_executor, interval_ticks_on_virtual_time) {
    virtual_executor ex;
    auto ticks = stepwright::interval(ex, 1s);
    std::vector<stepwright::timer_service::clock::duration> at;
    auto consumer = stepwright::for_each(ticks, [&](std::uint64_t) {
        at.push_back(ex.now() - origin);
        return at.size() < 3;
    });
    test_support::receiver done;
    consumer.start(done.callback());
    ex.run_until_quiescent();
    EXPECT_EQ(done.calls, 1);
    EXPECT_EQ(at, (std::vector<stepwright::timer_service::clock::duration>{1s, 2s, 3s}));
}

TEST(virtual_executor, regression_merge_no_head_of_line_blocking) {
    // The merge head-of-line case on virtual time: a 10 s and a 1 s interval
    // produce items at 1 s and 2 s, not after the slow input's first tick.
    virtual_executor ex;
    auto slow = stepwright::interval(ex, 10s);
    auto fast = stepwright::interval(ex, 1s);
    stepwright::merge m(slow, fast);
    std::vector<stepwright::timer_service::clock::duration> at;
    auto consumer = stepwright::for_each(m, [&](std::uint64_t) {
        at.push_back(ex.now() - origin);
        return at.size() < 2;
    });
    test_support::receiver done;
    consumer.start(done.callback());
    ex.run_until_quiescent();
    EXPECT_EQ(done.calls, 1);
    EXPECT_FALSE(done.result);
    EXPECT_EQ(at, (std::vector<stepwright::timer_service::clock::duration>{1s, 2s}));
    EXPECT_EQ(ex.now() - origin, 2s);
    EXPECT_EQ(ex.scheduled_timers(), 0U); // the slow input's wait was cancelled
}

namespace {
// A machine bound to the executor that sleeps n times on virtual time.
struct sleeper : stepwright::machine<sleeper> {
    explicit sleeper(virtual_executor& ex, stepwright::timer_service::clock::duration d, int n)
        : ex_(&ex)
        , wait_(ex, d)
        , remaining_(n) {
        bind(ex);
    }
    step begin() {
        if (!ex_->running_in_this_thread()) {
            ++off_executor;
        }
        if (remaining_-- == 0) {
            return done();
        }
        return after<&sleeper::begin>(wait_);
    }
    int off_executor = 0;

private:
    virtual_executor* ex_;
    stepwright::delay wait_;
    int remaining_;
};
} // namespace

TEST(virtual_executor, run_until_quiescent_terminates) {
    virtual_executor ex(schedule::random, 7);
    sleeper a(ex, 3ms, 5);
    sleeper b(ex, 7ms, 4);
    test_support::receiver done_a;
    test_support::receiver done_b;
    a.start(done_a.callback());
    b.start(done_b.callback());
    const auto count = ex.run_until_quiescent();
    EXPECT_EQ(count, 9U); // timer fires (the machines continue inline)
    EXPECT_EQ(done_a.calls, 1);
    EXPECT_EQ(done_b.calls, 1);
    EXPECT_EQ(ex.now() - origin, 28ms);
    EXPECT_EQ(ex.pending(), 0U);
    EXPECT_EQ(ex.scheduled_timers(), 0U);
    EXPECT_EQ(b.off_executor, 1); // only begin(), on the starting thread
    EXPECT_EQ(ex.run_until_quiescent(), 0U);
    EXPECT_EQ(ex.run_for(1s), 0U);
    EXPECT_EQ(ex.now() - origin, 1028ms);
}

TEST(virtual_executor, timer_cancel_semantics) {
    virtual_executor ex;
    std::vector<int> trace;
    timer_probe later;
    later.trace = &trace;
    later.id = 1;
    ex.schedule(later, origin + 1s);
    EXPECT_TRUE(ex.cancel(later)); // scheduled
    EXPECT_FALSE(ex.cancel(later));
    timer_probe now;
    now.trace = &trace;
    now.id = 2;
    ex.schedule(now, origin);
    EXPECT_EQ(ex.pending(), 1U);
    EXPECT_TRUE(ex.cancel(now)); // due but not run
    EXPECT_EQ(ex.pending(), 0U);
    ex.schedule(now, origin);
    ex.run_until_idle();
    EXPECT_FALSE(ex.cancel(now)); // fired
    EXPECT_EQ(trace, (std::vector<int>{2}));

    // From inside its own callback: false, without waiting.
    struct self_cancel : stepwright::timer_service::entry {
        virtual_executor* ex = nullptr;
        bool result = true;
    } own;
    own.ex = &ex;
    own.fire = [](stepwright::timer_service::entry* e) noexcept {
        auto& self = static_cast<self_cancel&>(*e);
        self.result = self.ex->cancel(self);
    };
    ex.schedule(own, origin + 1ms);
    ex.run_for(1ms);
    EXPECT_FALSE(own.result);

    // Equal deadlines fire in scheduling order.
    std::array<timer_probe, 4> same;
    for (std::size_t i = 0; i < same.size(); ++i) {
        same[i].trace = &trace;
        same[i].id = 10 + static_cast<int>(i);
        ex.schedule(same[i], ex.now() + 5ms);
    }
    ex.run_for(5ms);
    EXPECT_EQ(trace, (std::vector<int>{2, 10, 11, 12, 13}));
}

TEST(virtual_executor, running_in_this_thread_and_foreign_completions) {
    virtual_executor ex;
    EXPECT_FALSE(ex.running_in_this_thread());
    bool inside = false;
    ex.post([&] { inside = ex.running_in_this_thread(); });
    ex.run_one();
    EXPECT_TRUE(inside);

    // A bound machine whose child completes on another thread is posted, and
    // continues only when the test runs the executor.
    struct waiter : stepwright::machine<waiter> {
        test_support::async_child* child = nullptr;
        virtual_executor* ex = nullptr;
        bool continued_inside = false;
        step begin() { return after<&waiter::resumed>(*child); }
        step resumed() {
            continued_inside = ex->running_in_this_thread();
            return done();
        }
    } m;
    test_support::async_child child;
    m.child = &child;
    m.ex = &ex;
    m.bind(ex);
    test_support::receiver done;
    m.start(done.callback());
    std::thread([&] { child.finish(); }).join();
    EXPECT_EQ(done.calls, 0);
    EXPECT_EQ(ex.pending(), 1U);
    ex.run_until_idle();
    EXPECT_EQ(done.calls, 1);
    EXPECT_TRUE(m.continued_inside);
}

TEST(virtual_executor, posts_from_other_threads_are_safe) {
    virtual_executor ex;
    constexpr std::size_t per_thread = 2'000;
    std::atomic<std::size_t> ran{0};
    std::vector<std::vector<stepwright::work_item>> items;
    items.reserve(4);
    for (int t = 0; t < 4; ++t) {
        items.emplace_back(per_thread);
    }
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            auto& mine = items[static_cast<std::size_t>(t)];
            for (std::size_t i = 0; i < per_thread; ++i) {
                mine[i].run = [](stepwright::work_item*) noexcept {};
                ex.post(mine[i]);
            }
        });
    }
    std::size_t count = 0;
    while (count < 4 * per_thread) {
        count += ex.run_until_idle();
        ran.store(count);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(ex.run_until_idle(), 0U);
    EXPECT_EQ(ran.load(), 4 * per_thread);
}

namespace {
// The cancellation race of the seed sweep: a machine bound to the executor defers
// once, then waits on an event with its token. A due timer sets the event and a
// posted item requests cancellation; the scheduler orders the three.
struct race_machine : stepwright::machine<race_machine> {
    explicit race_machine(virtual_executor& ex, stepwright::event& ev, std::vector<char>& trace)
        : wait_(ev)
        , trace_(&trace) {
        bind(ex);
    }
    step begin() { return defer<&race_machine::wait>(); }
    step wait() {
        trace_->push_back('w');
        return after<&race_machine::woken>(wait_);
    }
    step woken(const stepwright::error& e) { return fail(e); }

private:
    stepwright::event::waiter wait_;
    std::vector<char>* trace_;
};

struct sweep_result {
    std::vector<char> trace;
    stepwright::error result;
    int calls = 0;
    bool quiescent = false;
};

sweep_result run_race(std::uint64_t seed) {
    virtual_executor ex(schedule::random, seed);
    stepwright::event ev;
    sweep_result out;
    race_machine m(ex, ev, out.trace);
    stepwright::cancel_source source;
    struct setter : stepwright::timer_service::entry {
        stepwright::event* ev = nullptr;
        std::vector<char>* trace = nullptr;
    } set_timer;
    set_timer.ev = &ev;
    set_timer.trace = &out.trace;
    set_timer.fire = [](stepwright::timer_service::entry* e) noexcept {
        auto& self = static_cast<setter&>(*e);
        self.trace->push_back('s');
        self.ev->set();
    };
    ex.schedule(set_timer, ex.now());
    ex.post([&] {
        out.trace.push_back('c');
        source.request_cancel();
    });
    struct sink {
        sweep_result* out;
        void finish(const stepwright::error& e) noexcept {
            out->result = e;
            ++out->calls;
        }
    } receiver{&out};
    m.start(stepwright::completion::bind<&sink::finish>(&receiver), source.token());
    ex.run_until_quiescent();
    out.quiescent = ex.pending() == 0 && ex.scheduled_timers() == 0;
    return out;
}
} // namespace

TEST(virtual_executor, regression_seed_sweep_cancellation_race) {
    const int seeds = test_support::stress_iterations(1'000);
    int ok = 0;
    int cancelled = 0;
    for (int i = 0; i < seeds; ++i) {
        const auto seed = static_cast<std::uint64_t>(i);
        const sweep_result r = run_race(seed);
        ASSERT_EQ(r.calls, 1) << "seed " << seed;
        ASSERT_TRUE(r.quiescent) << "seed " << seed;
        ASSERT_EQ(r.trace.size(), 3U) << "seed " << seed;
        // The wait succeeds iff the event was set before the wait started or
        // before cancellation was requested.
        const auto pos = [&](char c) { return std::ranges::find(r.trace, c) - r.trace.begin(); };
        const bool expect_ok = pos('s') < pos('w') || pos('s') < pos('c');
        if (expect_ok) {
            ASSERT_FALSE(r.result)
                << "seed " << seed << " trace " << std::string(r.trace.begin(), r.trace.end());
            ++ok;
        } else {
            ASSERT_TRUE(r.result.is(stepwright::errc::cancelled))
                << "seed " << seed << " trace " << std::string(r.trace.begin(), r.trace.end());
            ++cancelled;
        }
        if (i < 20) {
            const sweep_result again = run_race(seed);
            ASSERT_EQ(again.trace, r.trace) << "seed " << seed << " did not replay";
        }
    }
    std::cout << "seeds=" << seeds << " ok=" << ok << " cancelled=" << cancelled << '\n';
    EXPECT_GT(ok, 0);
    EXPECT_GT(cancelled, 0);
}
