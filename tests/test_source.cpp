#include <gtest/gtest.h>

#include "stream_support.hpp"
#include <memory>
#include <string>

namespace {
struct tracked {
    static inline int live = 0;
    int value = 0;
    explicit tracked(int v) noexcept : value(v) { ++live; }
    tracked(tracked&& other) noexcept : value(other.value) { ++live; }
    tracked(const tracked& other) noexcept : value(other.value) { ++live; }
    tracked& operator=(const tracked&) = delete;
    tracked& operator=(tracked&&) = delete;
    ~tracked() { --live; }
};

struct failing : stepwright::source_machine<failing, int> {
    int emitted = 0;
    step begin() {
        if (emitted < 2) {
            return emit<&failing::begin>(emitted++);
        }
        return fail(stepwright::error::make(stepwright::errc::closed, "boom"));
    }
};
} // namespace

TEST(pull, states_and_accessors) {
    auto item = stepwright::pull<int>::of(7);
    EXPECT_EQ(item.status(), stepwright::pull<int>::state::item);
    EXPECT_TRUE(item.has_item());
    EXPECT_EQ(item.value(), 7);
    const auto& constant = item;
    EXPECT_EQ(constant.value(), 7);
    EXPECT_TRUE(stepwright::pull<int>::pending().is_pending());
    EXPECT_TRUE(stepwright::pull<int>::end().at_end());
    EXPECT_TRUE(stepwright::pull<int>::failure({}).at_end());
    auto failed = stepwright::pull<int>::failure(stepwright::error::make(stepwright::errc::closed));
    EXPECT_TRUE(failed.is_failed());
    EXPECT_FALSE(failed.has_item());
    EXPECT_TRUE(failed.failure_reason().is(stepwright::errc::closed));
}

// GCC 14 at -O3 reports the union-held item of a moved pull as maybe
// uninitialized (a known false-positive class, as with std::optional).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
TEST(pull, move_copy_and_destruction) {
    {
        auto a = stepwright::pull<tracked>::of(tracked(1));
        EXPECT_EQ(tracked::live, 1);
        auto b = std::move(a);
        EXPECT_EQ(b.value().value, 1);
        auto c = b;
        EXPECT_EQ(tracked::live, 3);
        c = stepwright::pull<tracked>::end();
        EXPECT_EQ(tracked::live, 2);
        c = std::move(b);
        EXPECT_EQ(std::move(c).value().value, 1);
        auto e = stepwright::pull<tracked>::failure(
            stepwright::error::make(stepwright::errc::cancelled));
        auto f = e;
        auto g = std::move(e);
        EXPECT_TRUE(f.failure_reason().is(stepwright::errc::cancelled));
        EXPECT_TRUE(g.failure_reason().is(stepwright::errc::cancelled));
        auto& self = g;
        g = std::move(self);
        EXPECT_TRUE(g.is_failed());
    }
    EXPECT_EQ(tracked::live, 0);
    auto text = stepwright::pull<std::string>::of(std::string(64, 'x'));
    const std::string moved = std::move(text).value();
    EXPECT_EQ(moved.size(), 64U);
}

TEST(source, end_and_failure_are_sticky) {
    auto values = stepwright::just(1, 2);
    EXPECT_EQ(stream_support::drain<int>(values), (std::vector<int>{1, 2}));
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(values.try_pull().at_end());
    }
    failing f;
    stepwright::error reason;
    EXPECT_EQ(stream_support::drain<int>(f, &reason), (std::vector<int>{0, 1}));
    ASSERT_TRUE(reason.is(stepwright::errc::closed));
    for (int i = 0; i < 3; ++i) {
        auto again = f.try_pull();
        ASSERT_TRUE(again.is_failed());
        EXPECT_EQ(again.failure_reason().code(), reason.code());
        EXPECT_EQ(again.failure_reason().message(), "boom");
    }
    stepwright::channel<int> ch;
    ch.close(stepwright::error::make(stepwright::errc::timed_out));
    EXPECT_TRUE(ch.try_pull().failure_reason().is(stepwright::errc::timed_out));
    EXPECT_TRUE(ch.try_pull().failure_reason().is(stepwright::errc::timed_out));
}

TEST(source, ready_op_is_restartable) {
    stepwright::channel<int> ch;
    auto ready = ch.ready();
    stepwright::cancel_source source;
    for (int i = 0; i < 100; ++i) {
        test_support::receiver r;
        ready.start(r.callback(), source.token());
        EXPECT_EQ(r.calls, 0);
        EXPECT_TRUE(ch.try_send(i));
        EXPECT_EQ(r.calls, 1);
        EXPECT_FALSE(r.result);
        test_support::receiver sync;
        ready.start(sync.callback()); // already ready: synchronous
        EXPECT_EQ(sync.calls, 1);
        EXPECT_EQ(ch.try_pull().value(), i);
    }
    test_support::receiver cancelled;
    {
        stepwright::cancel_source local;
        ready.start(cancelled.callback(), local.token());
        local.request_cancel();
    }
    EXPECT_TRUE(cancelled.result.is(stepwright::errc::cancelled));
}

TEST(source, regression_readiness_wait_assertions) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::channel<int> ch;
            test_support::receiver a;
            test_support::receiver b;
            ch.on_ready(a.callback(), {});
            ch.on_ready(b.callback(), {});
        },
        "concurrent readiness waits");
    EXPECT_DEATH(
        {
            stepwright::channel<int> ch;
            test_support::receiver a;
            ch.on_ready(a.callback(), {});
            static_cast<void>(ch.try_pull());
        },
        "outstanding readiness wait");
    EXPECT_DEATH(
        {
            auto ch = std::make_unique<stepwright::channel<int>>();
            test_support::receiver a;
            ch->on_ready(a.callback(), {});
            ch.reset();
        },
        "destroying a source with an outstanding readiness wait");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
