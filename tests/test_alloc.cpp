#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "test_support.hpp"
#include <barrier>

TEST(alloc, regression_zero_alloc_then_and_sync_child) {
    struct op_type : stepwright::machine<op_type> {
        test_support::sync_child child;
        int remaining = 0;
        step begin() {
            if (remaining-- == 0) {
                return done();
            }
            return then<&op_type::child_step>();
        }
        step child_step() { return after<&op_type::begin>(child); }
    } op;
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        test_support::receiver receiver;
        op.remaining = test_support::stress_iterations(1000);
        EXPECT_EQ(count_allocations([&] { op.start(receiver.callback(), token); }), 0U);
        EXPECT_EQ(receiver.calls, 1);
    }
}
TEST(alloc, regression_zero_alloc_async_child) {
    struct op_type : stepwright::machine<op_type> {
        test_support::async_child child;
        step begin() { return forward(child); }
    } op;
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        test_support::receiver receiver;
        EXPECT_EQ(count_allocations([&] {
                      op.start(receiver.callback(), token);
                      op.child.finish();
                  }),
                  0U);
        EXPECT_EQ(receiver.calls, 1);
    }
}
TEST(alloc, regression_zero_alloc_park_wake_defer) {
    struct op_type : stepwright::machine<op_type> {
        step begin() { return park<&op_type::resumed>(); }
        step resumed() { return defer<&op_type::finish>(); }
        step finish() { return done(); }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        for (bool conditional : {false, true}) {
            test_support::receiver receiver;
            bool accepted = true;
            EXPECT_EQ(count_allocations([&] {
                          op.start(receiver.callback(), token);
                          if (conditional) {
                              accepted = op.wake_if_parked();
                          } else {
                              op.wake();
                          }
                          ex.run_all();
                      }),
                      0U);
            EXPECT_TRUE(accepted);
            EXPECT_EQ(receiver.calls, 1);
        }
    }
}
TEST(alloc, single_error_payload_and_intrusive_registration) {
    stepwright::error e;
    EXPECT_EQ(count_allocations([&] {
                  e = stepwright::error::make(stepwright::errc::closed,
                                              "inline message longer than SSO capacity");
              }),
              1U);
    EXPECT_EQ(count_allocations([&] {
                  auto copy = e;
                  copy = {};
                  e.add_context("unique");
              }),
              0U);
    stepwright::cancel_source source;
    EXPECT_EQ(
        count_allocations([&] { stepwright::cancel_callback callback(source.token(), [] {}); }),
        0U);
}
TEST(alloc, regression_zero_alloc_foreign_thread_child) {
    struct op_type : stepwright::machine<op_type> {
        test_support::async_child child;
        step begin() { return after<&op_type::finish>(child); }
        step finish() { return done(); }
    } op;
    std::barrier rendezvous(2);
    std::thread worker([&] {
        rendezvous.arrive_and_wait(); // Finish pthread/sanitizer startup before counting.
        for (int i = 0; i < 2; ++i) {
            rendezvous.arrive_and_wait();
            op.child.finish();
            rendezvous.arrive_and_wait();
        }
    });
    rendezvous.arrive_and_wait();
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        test_support::receiver receiver;
        EXPECT_EQ(count_allocations([&] {
                      op.start(receiver.callback(), token);
                      rendezvous.arrive_and_wait();
                      receiver.wait();
                      rendezvous.arrive_and_wait();
                  }),
                  0U);
        EXPECT_EQ(receiver.calls, 1);
    }
    worker.join();
}
