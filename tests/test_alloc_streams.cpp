#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "stream_support.hpp"
#include <array>
#include <chrono>

using namespace std::chrono_literals;

namespace {
// Run fn once to warm up (buffer growth, lazily created statics, thread locals),
// then require that a second run performs no allocation.
template <class F>
void expect_steady_state_zero_alloc(F&& fn, long expected) {
    EXPECT_EQ(fn(), expected);
    long result = 0;
    EXPECT_EQ(count_allocations([&] { result = fn(); }), 0U);
    EXPECT_EQ(result, expected);
}

struct counter : stepwright::source_machine<counter, int> {
    int next = 0;
    int limit = 0;
    step begin() { return next == limit ? end() : emit<&counter::begin>(next++); }
};
struct with_child : stepwright::source_machine<with_child, int> {
    stepwright::task* child = nullptr;
    int next = 0;
    step begin() { return after<&with_child::got>(*child); }
    step got() { return emit<&with_child::begin>(next++); }
};
} // namespace

TEST(alloc_streams, regression_zero_alloc_channel) {
    stepwright::channel<int> bounded(4);
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        expect_steady_state_zero_alloc(
            [&] {
                long sum = 0;
                for (int i = 0; i < 100; ++i) {
                    bounded.try_send(i);
                    sum += bounded.try_pull().value();
                }
                // Consumer wait woken by a send_op; then a waiting sender.
                test_support::receiver ready;
                bounded.on_ready(ready.callback(), token);
                auto op = bounded.send(1);
                test_support::receiver sent;
                op.start(sent.callback(), token);
                sum += bounded.try_pull().value() + ready.calls + sent.calls;
                for (int i = 0; i < 4; ++i) {
                    bounded.try_send(i);
                }
                auto waiting = bounded.send(7);
                test_support::receiver admitted;
                waiting.start(admitted.callback(), token);
                while (bounded.try_pull().has_item()) {
                    ++sum;
                }
                return sum + admitted.calls;
            },
            4950 + 1 + 2 + 5 + 1);
    }
    stepwright::channel<int> unbounded;
    expect_steady_state_zero_alloc(
        [&] {
            long sum = 0;
            for (int i = 0; i < 1000; ++i) {
                unbounded.try_send(i);
            }
            while (true) {
                auto next = unbounded.try_pull();
                if (!next.has_item()) {
                    break;
                }
                sum += next.value();
            }
            return sum;
        },
        499500);
}

TEST(alloc_streams, regression_zero_alloc_readiness_cancellation) {
    stepwright::channel<int> ch;
    std::array<stepwright::cancel_source, 2> sources;
    std::size_t index = 0;
    // A requested source cannot be reused publicly, so alternate fresh sources
    // created outside the counted region.
    const auto run = [&] {
        test_support::receiver ready;
        ch.on_ready(ready.callback(), sources[index].token());
        sources[index].request_cancel();
        return static_cast<long>(ready.result.is(stepwright::errc::cancelled));
    };
    EXPECT_EQ(run(), 1);
    index = 1;
    long result = 0;
    EXPECT_EQ(count_allocations([&] { result = run(); }), 0U);
    EXPECT_EQ(result, 1);
}

TEST(alloc_streams, regression_zero_alloc_merge) {
    stepwright::channel<int> a;
    stepwright::channel<int> b;
    auto always = stepwright::generate([]() -> std::optional<int> { return 1; });
    stepwright::merge m(a, b);
    stepwright::channel<int> idle;
    stepwright::merge ready_inputs(always, idle);
    expect_steady_state_zero_alloc(
        [&] {
            long sum = 0;
            for (int i = 0; i < 100; ++i) {
                sum += ready_inputs.try_pull().value();
            }
            // A readiness round: both inputs armed, a wins, b's wait is cancelled.
            for (int i = 0; i < 10; ++i) {
                test_support::receiver ready;
                m.on_ready(ready.callback(), {});
                a.try_send(i);
                sum += ready.calls + m.try_pull().value();
            }
            return sum;
        },
        100 + 10 + 45);
}

TEST(alloc_streams, regression_zero_alloc_broadcast) {
    stepwright::broadcast<int> b(4);
    auto s1 = b.subscribe();
    auto s2 = b.subscribe();
    expect_steady_state_zero_alloc(
        [&] {
            long sum = 0;
            for (int i = 0; i < 100; ++i) {
                b.try_publish(i);
                sum += s1.try_pull().value() + s2.try_pull().value();
            }
            // A waiting subscriber, then a waiting publisher released by the slowest.
            test_support::receiver ready;
            s1.on_ready(ready.callback(), {});
            for (int i = 0; i < 4; ++i) {
                b.try_publish(i);
            }
            auto op = b.publish(9);
            test_support::receiver published;
            op.start(published.callback());
            while (s1.try_pull().has_item()) {
            }
            while (s2.try_pull().has_item()) {
            }
            while (s1.try_pull().has_item()) {
            }
            return sum + ready.calls + published.calls;
        },
        9900 + 2);
}

TEST(alloc_streams, regression_zero_alloc_source_machine_and_for_each) {
    test_support::async_child child;
    with_child w;
    w.child = &child;
    expect_steady_state_zero_alloc(
        [&] {
            counter c; // a finished source stays finished: one per run
            c.limit = 1000;
            long sum = 0;
            auto drain = stepwright::for_each(c, [&](int v) { sum += v; });
            test_support::receiver done;
            drain.start(done.callback());
            // Asynchronous resumption: pending, readiness, child completion.
            for (int i = 0; i < 10; ++i) {
                static_cast<void>(w.try_pull());
                test_support::receiver ready;
                w.on_ready(ready.callback(), {});
                child.finish();
                sum += ready.calls;
                static_cast<void>(w.try_pull());
            }
            return sum + done.calls;
        },
        499500 + 10 + 1);
    auto g = stepwright::generate([n = 0]() mutable -> std::optional<int> { return n++ % 7; });
    expect_steady_state_zero_alloc(
        [&] {
            long sum = 0;
            int left = 1000;
            auto drain = stepwright::for_each(g, [&](int v) {
                sum += v;
                return --left > 0;
            });
            test_support::receiver done;
            drain.start(done.callback());
            return static_cast<long>(done.calls) + (sum > 0 ? 1 : 0);
        },
        2);
}

TEST(alloc_streams, regression_zero_alloc_interval) {
    stepwright::testing::manual_timer_service timers;
    auto ticks = stepwright::interval(timers, 1ms);
    expect_steady_state_zero_alloc(
        [&] {
            long count = 0;
            for (int i = 0; i < 10; ++i) {
                test_support::receiver ready;
                ticks.on_ready(ready.callback(), {});
                timers.advance(1ms);
                count += ready.calls + (ticks.try_pull().has_item() ? 1 : 0);
            }
            return count;
        },
        20);
}
