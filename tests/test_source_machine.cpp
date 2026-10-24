#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <exception>
#include <memory>
#include <optional>
#include <thread>

namespace {
struct counter : stepwright::source_machine<counter, int> {
    int next = 0;
    int limit = 0;
    int begins = 0;
    test_support::depth_probe* probe = nullptr;
    step begin() {
        ++begins;
        return then<&counter::produce>();
    }
    step produce() {
        if (probe) {
            probe->sample();
        }
        if (next == limit) {
            return end();
        }
        return emit<&counter::produce>(next++);
    }
};

// Emits after a child for every item.
struct with_child : stepwright::source_machine<with_child, int> {
    stepwright::task* child = nullptr;
    int next = 0;
    int limit = 0;
    step begin() {
        if (next == limit) {
            return end();
        }
        return after<&with_child::got>(*child);
    }
    step got(const stepwright::error& e) {
        if (e) {
            return fail(e);
        }
        return emit<&with_child::begin>(next++);
    }
};
// No-argument continuation: a child failure fails the stream.
struct strict_child : stepwright::source_machine<strict_child, int> {
    stepwright::task* child = nullptr;
    step begin() { return after<&strict_child::got>(*child); }
    step got() { return emit<&strict_child::begin>(1); }
};

struct parker : stepwright::source_machine<parker, int> {
    int next = 0;
    step begin() { return park<&parker::woken>(); }
    step woken(const stepwright::error& e) {
        if (e) {
            return fail(e);
        }
        return emit<&parker::begin>(next++);
    }
};

struct deferring : stepwright::source_machine<deferring, int> {
    int next = 0;
    step begin() { return defer<&deferring::resumed>(); }
    step resumed() {
        if (next == 3) {
            return end();
        }
        return emit<&deferring::begin>(next++);
    }
};

struct hooked : stepwright::source_machine<hooked, int> {
    std::atomic<int> hooks{0};
    std::string context;
    void on_cancel() noexcept { hooks.fetch_add(1); }
    void annotate(stepwright::error& e) { e.add_context("hooked-annotation"); }
    step begin() { return park<&hooked::woken>(); }
    step woken(const stepwright::error& e) { return fail(e); }
};

// Completes through the calling thread's trampoline: queued (asynchronous) when
// started inside a trampoline-delivered completion or handoff_scope.
struct handoff_child : stepwright::task, stepwright::work_item {
    stepwright::completion receiver;
    handoff_child() {
        run = [](stepwright::work_item* item) noexcept {
            if (!item) {
                std::terminate();
            }
            const auto done = static_cast<handoff_child*>(item)->receiver;
            done({});
        };
    }
    void on_start(stepwright::completion done, stepwright::cancel_token) override {
        receiver = done;
        stepwright::detail::handoff(*this);
    }
};
} // namespace

TEST(source_machine, emits_in_order_and_ends) {
    counter c;
    c.limit = 5;
    EXPECT_EQ(stream_support::drain<int>(c), (std::vector<int>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(c.try_pull().at_end());
    EXPECT_TRUE(c.try_pull().at_end());
    EXPECT_EQ(c.begins, 1);
}

TEST(source_machine, lazy_until_first_pull) {
    counter c;
    c.limit = 2;
    EXPECT_EQ(c.begins, 0);
    test_support::receiver ready;
    c.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(c.begins, 1);
    EXPECT_EQ(c.try_pull().value(), 0); // produced by on_ready, held for the pull
    test_support::receiver again;
    c.on_ready(again.callback(), {});
    c.on_ready(again.callback(), {}); // already held: still synchronous
    EXPECT_EQ(again.calls, 2);
    EXPECT_EQ(c.try_pull().value(), 1);
    EXPECT_TRUE(c.try_pull().at_end());
    test_support::receiver at_end;
    c.on_ready(at_end.callback(), {});
    EXPECT_EQ(at_end.calls, 1);
    counter unused; // destroying a never-pulled source is fine
    EXPECT_EQ(unused.begins, 0);
}

TEST(source_machine, waits_on_child_and_signals_readiness) {
    test_support::async_child child;
    with_child s;
    s.child = &child;
    s.limit = 3;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(s.try_pull().is_pending());
        EXPECT_TRUE(s.try_pull().is_pending()); // still suspended
        test_support::receiver ready;
        s.on_ready(ready.callback(), {});
        EXPECT_EQ(ready.calls, 0);
        child.finish();
        EXPECT_EQ(ready.calls, 1);
        EXPECT_EQ(s.try_pull().value(), i);
    }
    EXPECT_TRUE(s.try_pull().at_end());
    // Child completing before the readiness registration: synchronous readiness.
    with_child t;
    t.child = &child;
    t.limit = 1;
    EXPECT_TRUE(t.try_pull().is_pending());
    child.finish();
    test_support::receiver ready;
    t.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(t.try_pull().value(), 0);
    // on_ready drives a fresh run that suspends, then the child completes.
    with_child u;
    u.child = &child;
    u.limit = 1;
    test_support::receiver later;
    u.on_ready(later.callback(), {});
    EXPECT_EQ(later.calls, 0);
    child.finish();
    EXPECT_EQ(later.calls, 1);
    EXPECT_EQ(u.try_pull().value(), 0);
    EXPECT_TRUE(u.try_pull().at_end());
}

TEST(source_machine, child_failure_is_sticky_with_context) {
    test_support::async_child child;
    strict_child s;
    s.child = &child;
    EXPECT_TRUE(s.try_pull().is_pending());
    test_support::receiver ready;
    s.on_ready(ready.callback(), {});
    child.finish(stepwright::error::make(stepwright::errc::timed_out));
    EXPECT_EQ(ready.calls, 1);
    auto failed = s.try_pull();
    ASSERT_TRUE(failed.is_failed());
    EXPECT_TRUE(failed.failure_reason().is(stepwright::errc::timed_out));
    EXPECT_EQ(failed.failure_reason().context().size(), 1U);
    EXPECT_TRUE(s.try_pull().failure_reason().is(stepwright::errc::timed_out));
    test_support::sync_child ok;
    strict_child sync;
    sync.child = &ok;
    EXPECT_EQ(sync.try_pull().value(), 1);
    ok.result = stepwright::error::make(stepwright::errc::closed);
    EXPECT_TRUE(sync.try_pull().failure_reason().is(stepwright::errc::closed));
}

TEST(source_machine, park_and_wake) {
    parker p;
    p.wake(); // buffered before the first park
    EXPECT_EQ(p.try_pull().value(), 0);
    EXPECT_TRUE(p.try_pull().is_pending());
    EXPECT_TRUE(p.wake_if_parked()); // parked now: accepted
    EXPECT_EQ(p.try_pull().value(), 1);
    EXPECT_TRUE(p.try_pull().is_pending());
    test_support::receiver ready;
    p.on_ready(ready.callback(), {});
    p.wake();
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(p.try_pull().value(), 2);
    EXPECT_TRUE(p.try_pull().is_pending());
    p.wake(stepwright::error::make(stepwright::errc::closed));
    EXPECT_TRUE(p.try_pull().failure_reason().is(stepwright::errc::closed));
    p.wake(); // dropped after the stream finished
    EXPECT_FALSE(p.wake_if_parked());
    EXPECT_TRUE(p.try_pull().is_failed());
}

TEST(source_machine, defer_runs_on_home_executor) {
    stepwright::manual_executor ex;
    deferring d;
    d.bind(ex);
    EXPECT_EQ(d.home(), &ex);
    EXPECT_TRUE(d.try_pull().is_pending());
    test_support::receiver ready;
    d.on_ready(ready.callback(), {});
    EXPECT_EQ(ready.calls, 0);
    EXPECT_EQ(ex.run_all(), 1U);
    EXPECT_EQ(ready.calls, 1);
    EXPECT_EQ(d.try_pull().value(), 0);
    stepwright::inline_executor now;
    deferring inline_defer;
    inline_defer.bind(now);
    EXPECT_EQ(stream_support::drain<int>(inline_defer), (std::vector<int>{0, 1, 2}));
}

TEST(source_machine, cancelling_readiness_keeps_child_and_state) {
    test_support::cancellable_child child;
    with_child s;
    s.child = &child;
    s.limit = 1;
    EXPECT_TRUE(s.try_pull().is_pending());
    stepwright::cancel_source consumer;
    test_support::receiver ready;
    s.on_ready(ready.callback(), consumer.token());
    consumer.request_cancel();
    EXPECT_TRUE(ready.result.is(stepwright::errc::cancelled));
    EXPECT_FALSE(child.received_token.can_be_cancelled()); // not the consumer's token
    EXPECT_TRUE(child.pending.load());
    EXPECT_TRUE(s.try_pull().is_pending());
    child.complete();
    EXPECT_EQ(s.try_pull().value(), 0);
    EXPECT_TRUE(s.try_pull().at_end());
}

TEST(source_machine, own_token_reaches_children_and_cancels_parks) {
    stepwright::cancel_source source;
    test_support::cancellable_child child;
    with_child s;
    s.child = &child;
    s.limit = 2;
    s.set_token(source.token());
    EXPECT_FALSE(s.cancel_requested());
    EXPECT_TRUE(s.try_pull().is_pending());
    EXPECT_TRUE(child.received_token.can_be_cancelled());
    test_support::receiver ready;
    s.on_ready(ready.callback(), {});
    source.request_cancel();
    EXPECT_EQ(ready.calls, 1);
    EXPECT_TRUE(s.try_pull().failure_reason().is(stepwright::errc::cancelled));

    stepwright::cancel_source park_source;
    parker p;
    p.set_token(park_source.token());
    EXPECT_TRUE(p.try_pull().is_pending());
    park_source.request_cancel();
    EXPECT_TRUE(p.try_pull().failure_reason().is(stepwright::errc::cancelled));
    parker late; // cancelled before it parks
    late.set_token(park_source.token());
    EXPECT_TRUE(late.try_pull().failure_reason().is(stepwright::errc::cancelled));

    stepwright::cancel_source hook_source;
    hooked h;
    h.set_token(hook_source.token());
    EXPECT_TRUE(h.try_pull().is_pending());
    hook_source.request_cancel();
    EXPECT_EQ(h.hooks.load(), 1);
    EXPECT_TRUE(h.try_pull().is_pending()); // the hook decides; nothing woke it
    h.wake(stepwright::error::make(stepwright::errc::closed));
    auto failed = h.try_pull();
    ASSERT_TRUE(failed.is_failed());
    ASSERT_EQ(failed.failure_reason().context().size(), 2U);
    EXPECT_STREQ(failed.failure_reason().context()[1], "hooked-annotation");
}

TEST(source_machine, regression_stack_one_million_emits) {
    const int count = 1'000'000;
    test_support::depth_probe probe;
    counter c;
    c.limit = count;
    c.probe = &probe;
    long long sum = 0;
    auto drain = stepwright::for_each(c, [&](int v) { sum += v; });
    test_support::receiver r;
    drain.start(r.callback());
    EXPECT_EQ(r.calls, 1);
    EXPECT_FALSE(r.result);
    EXPECT_EQ(sum, static_cast<long long>(count) * (count - 1) / 2);
    EXPECT_LT(probe.span(), 64U * 1024U);
}

TEST(source_machine, regression_stack_one_million_async_resumptions) {
    const int count = 1'000'000;
    handoff_child child;
    with_child s;
    s.child = &child;
    s.limit = count;
    test_support::depth_probe probe;
    int received = 0;
    auto drain = stepwright::for_each(s, [&](int v) {
        probe.sample();
        EXPECT_EQ(v, received);
        ++received;
    });
    test_support::receiver r;
    {
        // Inside a trampoline every child completes later: each item is an
        // asynchronous resumption that publishes and signals readiness.
        const stepwright::detail::handoff_scope scope;
        drain.start(r.callback());
        EXPECT_EQ(r.calls, 0);
    }
    EXPECT_EQ(r.calls, 1);
    EXPECT_EQ(received, count);
    EXPECT_LT(probe.span(), 64U * 1024U);
}

TEST(source_machine, resumes_from_other_threads) {
    const int count = test_support::stress_iterations(5'000);
    for (const bool bound : {false, true}) {
        test_support::async_child child;
        with_child s;
        s.child = &child;
        s.limit = count;
        stepwright::manual_executor home;
        if (bound) {
            s.bind(home);
        }
        std::atomic<bool> stop{false};
        std::thread home_thread([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (!home.run_one()) {
                    std::this_thread::yield();
                }
            }
            home.run_all();
        });
        std::atomic<int> started{0};
        std::thread completer([&] {
            for (int i = 0; i < count; ++i) {
                while (started.load(std::memory_order_acquire) <= i) {
                    std::this_thread::yield();
                }
                child.finish();
            }
        });
        for (int i = 0; i < count; ++i) {
            auto next = s.try_pull();
            if (next.is_pending()) {
                started.fetch_add(1, std::memory_order_release);
                next = stream_support::pull_blocking<int>(s);
            }
            ASSERT_EQ(next.value(), i);
        }
        started.fetch_add(1, std::memory_order_release);
        EXPECT_TRUE(s.try_pull().at_end());
        completer.join();
        stop.store(true, std::memory_order_release);
        home_thread.join();
    }
}

TEST(source_machine, wakes_from_other_threads) {
    const int count = test_support::stress_iterations(5'000);
    parker p;
    std::atomic<bool> done{false};
    std::thread waker([&] {
        while (!done.load(std::memory_order_acquire)) {
            p.wake_if_parked();
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < count; ++i) {
        ASSERT_EQ(stream_support::pull_blocking<int>(p).value(), i);
    }
    done.store(true, std::memory_order_release);
    waker.join();
}

TEST(source_machine, regression_destroy_suspended_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            test_support::async_child child;
            auto s = std::make_unique<with_child>();
            s->child = &child;
            s->limit = 1;
            static_cast<void>(s->try_pull());
            s.reset();
        },
        "destroying a suspended source_machine");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}

TEST(source_machine, regression_readiness_cancel_vs_publication) {
    const int rounds = test_support::stress_iterations(20'000);
    test_support::async_child child;
    with_child s;
    s.child = &child;
    s.limit = rounds;
    std::optional<stepwright::cancel_source> consumer;
    unsigned seed_a = 1;
    unsigned seed_b = 2;
    test_support::race_rounds race({
        [&] {
            test_support::jitter(seed_a, 3);
            child.finish();
        },
        [&] {
            test_support::jitter(seed_b, 3);
            consumer->request_cancel();
        },
    });
    int cancelled = 0;
    for (int r = 0; r < rounds; ++r) {
        consumer.emplace();
        ASSERT_TRUE(s.try_pull().is_pending());
        test_support::receiver ready;
        s.on_ready(ready.callback(), consumer->token());
        race.run_round();
        ready.wait();
        ASSERT_EQ(ready.calls, 1);
        cancelled += ready.result ? 1 : 0;
        ASSERT_EQ(s.try_pull().value(), r); // published either way
    }
    EXPECT_TRUE(s.try_pull().at_end());
    std::cout << "cancelled " << cancelled << " / " << rounds << '\n';
}
