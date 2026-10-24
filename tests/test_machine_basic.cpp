#include <gtest/gtest.h>

#include "test_support.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace {
struct basic : stepwright::machine<basic> {
    test_support::sync_child child;
    int mode = 0;
    int steps = 0;
    std::vector<int>* order = nullptr;
    step begin() {
        switch (mode) {
            case 0:
                return done();
            case 1:
                return fail(stepwright::error::make(stepwright::errc::closed));
            case 2:
                return then<&basic::with_arg>();
            case 3:
                return after<&basic::with_arg>(child);
            case 4:
                return after<&basic::no_arg>(child);
            case 5:
                return forward(child);
            default:
                return fail({});
        }
    }
    step with_arg(const stepwright::error& e) noexcept {
        ++steps;
        return fail(e);
    }
    step no_arg() {
        ++steps;
        return then<&basic::with_arg>();
    }
    void annotate(stepwright::error& e) { e.add_context("annotated"); }
    void on_complete(const stepwright::error&) noexcept {
        if (order) {
            order->push_back(1);
        }
    }
};
} // namespace

TEST(machine, done_fail_then_after_forward_restart) {
    for (int mode = 0; mode <= 6; ++mode) {
        for (bool failure : {false, true}) {
            basic op;
            op.mode = mode;
            if (failure) {
                op.child.result = stepwright::error::make(stepwright::errc::timed_out);
            }
            for (int restart = 0; restart < 2; ++restart) {
                test_support::receiver receiver;
                op.start(receiver.callback());
                EXPECT_EQ(receiver.calls, 1);
                const bool failed = mode == 1 || (failure && mode >= 3 && mode <= 5);
                EXPECT_EQ(static_cast<bool>(receiver.result), failed);
                if (failed) {
                    ASSERT_EQ(receiver.result.context().size(), 2U);
                    const auto frames = receiver.result.context();
                    if (!frames.data()) {
                        ADD_FAILURE() << "missing context storage";
                        continue;
                    }
                    const char* frame = frames[0];
                    if (!frame) {
                        ADD_FAILURE() << "missing machine context";
                        continue;
                    }
                    EXPECT_NE(std::string_view(frame).find("basic"), std::string_view::npos);
                    EXPECT_STREQ(receiver.result.context()[1], "annotated");
                }
            }
            if (mode == 4) {
                EXPECT_EQ(op.steps, failure ? 0 : 4);
            }
        }
    }
}
TEST(machine, hook_order_and_completion_bind) {
    basic op;
    std::vector<int> order;
    op.order = &order;
    op.start({.fn =
                  [](void* ctx, const stepwright::error&) noexcept {
                      static_cast<std::vector<int>*>(ctx)->push_back(2);
                  },
              .ctx = &order});
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_FALSE(stepwright::completion{});
}
TEST(machine, regression_completion_destroys_machine) {
    auto op = std::make_unique<basic>();
    op->start({.fn =
                   [](void* ctx, const stepwright::error&) noexcept {
                       static_cast<std::unique_ptr<basic>*>(ctx)->reset();
                   },
               .ctx = &op});
    EXPECT_FALSE(op);
}
TEST(machine, regression_async_completion_destroys_machine) {
    auto op = std::make_unique<test_support::parked>();
    op->start({.fn =
                   [](void* ctx, const stepwright::error&) noexcept {
                       static_cast<std::unique_ptr<test_support::parked>*>(ctx)->reset();
                   },
               .ctx = &op});
    op->wake();
    EXPECT_FALSE(op);
}
TEST(machine, regression_type_name_array_and_termination) {
    constexpr const char* name = stepwright::detail::type_name<basic>();
    static_assert(std::string_view(name).find("basic") != std::string_view::npos);
    EXPECT_EQ(name, stepwright::detail::type_name<basic>());
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): exercise brackets in compiler-generated type names.
    EXPECT_EQ(std::string_view(stepwright::detail::type_name<int[3]>()).back(), ']');
}
TEST(machine, park_no_arg_failure_skips_step) {
    struct op_type : stepwright::machine<op_type> {
        int calls = 0;
        step begin() { return park<&op_type::next_step>(); }
        step next_step() {
            ++calls;
            return done();
        }
    } op;
    test_support::receiver receiver;
    op.start(receiver.callback());
    op.wake(stepwright::error::make(stepwright::errc::closed));
    EXPECT_EQ(op.calls, 0);
    EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
    EXPECT_EQ(receiver.result.context().size(), 1U);
}
TEST(machine, regression_destroy_in_flight) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            test_support::parked op;
            test_support::receiver receiver;
            op.start(receiver.callback());
        },
        "destroying an in-flight machine");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
TEST(machine, regression_wake_on_idle_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            test_support::parked op;
            op.wake();
        },
        "wake on idle machine");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
TEST(machine, conditional_wake_is_only_for_current_park) {
    test_support::parked op;
    EXPECT_FALSE(op.wake_if_parked());
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_TRUE(op.wake_if_parked());
    EXPECT_FALSE(op.wake_if_parked());
    EXPECT_EQ(receiver.calls, 1);
    struct waiting : stepwright::machine<waiting> {
        test_support::async_child child;
        step begin() { return forward(child); }
    } parent;
    test_support::receiver child_receiver;
    parent.start(child_receiver.callback());
    EXPECT_FALSE(parent.wake_if_parked(stepwright::error::make(stepwright::errc::cancelled)));
    // Ordinary wakes are legal here, but forward() never parks: the unused wake
    // must be discarded without replacing the child's eventual success.
    parent.wake(stepwright::error::make(stepwright::errc::timed_out));
    parent.child.finish();
    EXPECT_EQ(child_receiver.calls, 1);
    EXPECT_FALSE(child_receiver.result);
}
TEST(machine, conditional_wake_rejects_pending_result) {
    stepwright::manual_executor ex;
    test_support::parked op;
    op.bind(ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_TRUE(op.wake_if_parked(stepwright::error::make(stepwright::errc::closed)));
    // The continuation is queued, so the first result is deterministically
    // still pending. A duplicate must not replace it or enqueue another item.
    EXPECT_FALSE(op.wake_if_parked(stepwright::error::make(stepwright::errc::timed_out)));
    EXPECT_EQ(ex.pending(), 1U);
    EXPECT_EQ(ex.run_all(), 1U);
    EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
    EXPECT_EQ(receiver.calls, 1);
}
TEST(machine, wake_buffered_across_child_wait) {
    struct request : stepwright::machine<request> {
        test_support::sync_child send;
        bool child_failed = false;
        bool child_result_correct = false;
        int replies = 0;
        step begin() {
            wake(stepwright::error::make(stepwright::errc::closed, "reply"));
            wake(stepwright::error::make(stepwright::errc::cancelled, "duplicate"));
            return after<&request::on_sent>(send);
        }
        step on_sent(const stepwright::error& e) {
            child_result_correct = child_failed ? e.is(stepwright::errc::broken_promise) : !e;
            return park<&request::on_reply>();
        }
        step on_reply(const stepwright::error& e) {
            ++replies;
            return fail(e);
        }
    };
    for (bool failed : {false, true}) {
        request op;
        op.child_failed = failed;
        if (failed) {
            op.send.result = stepwright::error::make(stepwright::errc::broken_promise);
        }
        test_support::receiver receiver;
        op.start(receiver.callback()); // Must not suspend on the buffered reply.
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
        EXPECT_EQ(receiver.result.message(), "reply");
        EXPECT_TRUE(op.child_result_correct);
        EXPECT_EQ(op.replies, 1);
        EXPECT_EQ(op.send.calls, 1);
    }
}
TEST(machine, wake_buffered_across_defer) {
    struct request : stepwright::machine<request> {
        bool deferred_result_empty = false;
        int replies = 0;
        step begin() {
            wake(stepwright::error::make(stepwright::errc::closed, "reply"));
            return defer<&request::on_sent>();
        }
        step on_sent(const stepwright::error& e) {
            deferred_result_empty = !e;
            return park<&request::on_reply>();
        }
        step on_reply(const stepwright::error& e) {
            ++replies;
            return fail(e);
        }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    EXPECT_EQ(ex.pending(), 1U);
    EXPECT_EQ(ex.run_all(), 1U);
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
    EXPECT_EQ(receiver.result.message(), "reply");
    EXPECT_TRUE(op.deferred_result_empty);
    EXPECT_EQ(op.replies, 1);
}
