#include <gtest/gtest.h>

#include "test_support.hpp"
#include <vector>

TEST(machine, defer_order_and_then) {
    struct deferred : stepwright::machine<deferred> {
        std::vector<int> order;
        step begin() {
            order.push_back(1);
            return defer<&deferred::second>();
        }
        step second(const stepwright::error& e) {
            EXPECT_FALSE(e);
            EXPECT_TRUE(home()->running_in_this_thread());
            order.push_back(3);
            return then<&deferred::third>();
        }
        step third() {
            order.push_back(4);
            return done();
        }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    op.order.push_back(2);
    EXPECT_EQ(ex.run_all(), 1U);
    EXPECT_EQ(op.order, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_EQ(receiver.calls, 1);
}
TEST(machine, regression_inline_defer_stack) {
    struct deferred : stepwright::machine<deferred> {
        int remaining = test_support::stress_iterations(100'000);
        step begin() { return remaining-- == 0 ? done() : defer<&deferred::begin>(); }
    } op;
    stepwright::inline_executor ex;
    op.bind(ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
}
TEST(machine, regression_early_wake_survives_defer) {
    struct deferred : stepwright::machine<deferred> {
        bool early = false;
        step begin() {
            if (early) {
                wake(stepwright::error::make(stepwright::errc::closed));
            }
            return defer<&deferred::next_step>();
        }
        step next_step(const stepwright::error& e) {
            EXPECT_FALSE(e); // Defer never consumes the upcoming park's error.
            return park<&deferred::finish>();
        }
        step finish(const stepwright::error& e) { return fail(e); }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    for (bool early : {false, true}) {
        op.early = early;
        test_support::receiver receiver;
        op.start(receiver.callback());
        if (!early) {
            op.wake(stepwright::error::make(stepwright::errc::closed));
        }
        EXPECT_EQ(receiver.calls, 0);
        EXPECT_EQ(ex.pending(), 1U);
        EXPECT_EQ(ex.run_all(), 1U);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
        EXPECT_EQ(receiver.calls, 1);
    }
}
