#include <gtest/gtest.h>

#include "test_support.hpp"

namespace {
template <int Mode>
struct chain : stepwright::machine<chain<Mode>> {
    using base = stepwright::machine<chain<Mode>>;
    using typename base::step;
    test_support::sync_child child;
    int remaining = test_support::stress_iterations(1'000'000);
    step begin() {
        if (remaining-- == 0) {
            return this->done();
        }
        if constexpr (Mode == 0) {
            return this->template then<&chain::begin>();
        }
        if constexpr (Mode == 1) {
            return this->template after<&chain::begin>(child);
        }
        this->wake();
        return this->template park<&chain::begin>();
    }
};
template <int Mode>
void run_chain() {
    chain<Mode> op;
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
    EXPECT_EQ(op.remaining, -1);
}
} // namespace
TEST(machine, regression_stack_then_million) {
    run_chain<0>();
}
TEST(machine, regression_stack_after_sync_million) {
    run_chain<1>();
}
TEST(machine, regression_stack_early_wake_million) {
    run_chain<2>();
}
