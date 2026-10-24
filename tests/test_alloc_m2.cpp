#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "test_support.hpp"
#include <chrono>

using namespace std::chrono_literals;

namespace {
// Run fn once to warm up lazily created state (internal cancel sources, thread
// locals), then require that a second run performs no allocation.
// fn returns a checksum (completion count) compared outside the counted region.
template <class F>
void expect_steady_state_zero_alloc(F&& fn, int expected) {
    EXPECT_EQ(fn(), expected);
    int result = 0;
    EXPECT_EQ(count_allocations([&] { result = fn(); }), 0U);
    EXPECT_EQ(result, expected);
}
} // namespace

TEST(alloc_m2, regression_zero_alloc_event_wait_set) {
    stepwright::event ev;
    auto early = ev.wait();
    auto late = ev.wait();
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        expect_steady_state_zero_alloc(
            [&] {
                test_support::receiver a;
                test_support::receiver b;
                early.start(a.callback(), token);
                ev.set();
                late.start(b.callback(), token);
                ev.reset();
                return a.calls + b.calls;
            },
            2);
    }
}

TEST(alloc_m2, regression_zero_alloc_mutex_lock_unlock) {
    stepwright::async_mutex m;
    auto first = m.lock();
    auto second = m.lock();
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        expect_steady_state_zero_alloc(
            [&] {
                test_support::receiver a;
                test_support::receiver b;
                first.start(a.callback(), token);  // uncontended
                second.start(b.callback(), token); // contended: queued
                m.unlock();                        // hand-off
                m.unlock();
                return a.calls + b.calls;
            },
            2);
    }
    stepwright::async_shared_mutex rw;
    auto reader = rw.lock_shared();
    auto writer = rw.lock();
    expect_steady_state_zero_alloc(
        [&] {
            test_support::receiver a;
            test_support::receiver b;
            reader.start(a.callback(), source.token());
            writer.start(b.callback(), source.token());
            rw.unlock_shared();
            rw.unlock();
            return a.calls + b.calls;
        },
        2);
}

TEST(alloc_m2, regression_zero_alloc_when_all_when_any) {
    test_support::sync_child a;
    test_support::sync_child b;
    test_support::async_child c;
    stepwright::when_all all(a, b, c);
    stepwright::when_any any(a, b, c);
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        expect_steady_state_zero_alloc(
            [&] {
                test_support::receiver r1;
                all.start(r1.callback(), token);
                c.finish();
                test_support::receiver r2;
                any.start(r2.callback(), token); // a wins and cancels the others
                c.finish();
                return r1.calls + r2.calls + (any.winner() == 0 ? 1 : 0);
            },
            3);
    }
}

TEST(alloc_m2, regression_zero_alloc_timers) {
    stepwright::testing::manual_timer_service manual;
    stepwright::steady_timer_service steady;
    struct noop : stepwright::timer_service::entry {
        noop() {
            fire = [](stepwright::timer_service::entry*) noexcept {};
        }
    } e;
    for (stepwright::timer_service* timers : {static_cast<stepwright::timer_service*>(&manual),
                                              static_cast<stepwright::timer_service*>(&steady)}) {
        expect_steady_state_zero_alloc(
            [&] {
                timers->schedule(e, timers->now() + 1h);
                return timers->cancel(e) ? 1 : 0;
            },
            1);
    }
}

TEST(alloc_m2, regression_zero_alloc_delay_and_with_deadline) {
    stepwright::testing::manual_timer_service timers;
    stepwright::delay sleep(timers, 5ms);
    test_support::async_child inner;
    stepwright::with_deadline guarded(inner, timers, 50ms);
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        expect_steady_state_zero_alloc(
            [&] {
                test_support::receiver r1;
                sleep.start(r1.callback(), token);
                timers.advance(5ms);
                test_support::receiver r2;
                guarded.start(r2.callback(), token);
                inner.finish();
                return r1.calls + r2.calls;
            },
            2);
    }
}

TEST(alloc_m2, regression_zero_alloc_shared_task_and_adapters) {
    test_support::async_child inner;
    stepwright::shared_task shared(inner);
    auto w1 = shared.wait();
    auto w2 = shared.wait();
    stepwright::manual_executor ex;
    stepwright::reschedule hop(ex);
    expect_steady_state_zero_alloc(
        [&] {
            test_support::receiver a;
            test_support::receiver b;
            w1.start(a.callback());
            w2.start(b.callback());
            inner.finish();
            shared.reset();
            test_support::receiver c;
            hop.start(c.callback());
            ex.run_all();
            return a.calls + b.calls + c.calls;
        },
        3);
}
