#include <stepwright/error.hpp>

#include <gtest/gtest.h>

#include "stress.hpp"
#include <array>
#include <string>
#include <thread>
#include <utility>

TEST(error, empty) {
    stepwright::error e;
    EXPECT_FALSE(e);
    EXPECT_EQ(e.code(), std::error_code{});
    EXPECT_TRUE(e.message().empty());
    EXPECT_TRUE(e.context().empty());
    EXPECT_EQ(e.dropped_context(), 0U);
    EXPECT_EQ(e.describe(), "ok");
    e.add_context("ignored");
    auto copy = e;
    auto moved = std::move(copy);
    e = moved;
    e = std::move(moved);
    EXPECT_FALSE(e);
    EXPECT_FALSE(stepwright::error::make(std::error_code{}));
}
TEST(error, codes_and_ownership) {
    std::error_code implicit = stepwright::errc::closed;
    EXPECT_STREQ(implicit.category().name(), "stepwright");
    for (const auto code : {stepwright::errc::cancelled, stepwright::errc::timed_out,
                            stepwright::errc::closed, stepwright::errc::broken_promise}) {
        auto e = stepwright::error::make(code, "detail");
        EXPECT_TRUE(e.is(code));
        EXPECT_EQ(e.message(), "detail");
        EXPECT_FALSE(e.code().message().empty());
        auto copy = e;
        auto moved = std::move(copy);
        EXPECT_FALSE(
            copy); // NOLINT(bugprone-use-after-move): moved-from emptiness is the contract.
        EXPECT_EQ(moved.code(), e.code());
        copy = moved;
        moved = std::move(copy);
        EXPECT_TRUE(moved);
    }
    EXPECT_EQ(stepwright::error::make(std::errc::invalid_argument).code(),
              std::make_error_code(std::errc::invalid_argument));
    EXPECT_EQ(stepwright::error::make(implicit).code(), implicit);
    EXPECT_EQ(stepwright::category().message(123), "unknown stepwright error");
}
TEST(error, context_cow_and_cap) {
    auto original = stepwright::error::make(stepwright::errc::timed_out, "no reply");
    original.add_context("fetch_op");
    auto copy = original;
    copy.add_context("client_op");
    EXPECT_EQ(original.context().size(), 1U);
    EXPECT_EQ(copy.describe(), "timed_out: no reply [fetch_op <- client_op]");
    for (int i = 0; i < 10; ++i) {
        copy.add_context("extra");
    }
    EXPECT_EQ(copy.context().size(), 8U);
    EXPECT_EQ(copy.dropped_context(), 4U);
    EXPECT_EQ(original.dropped_context(), 0U);
}
TEST(error, regression_concurrent_cow) {
    const auto original = stepwright::error::make(stepwright::errc::closed, "shared");
    std::array<std::thread, 8> threads;
    for (auto& thread : threads) {
        thread = std::thread([copy = original]() mutable {
            for (int i = 0; i < test_support::stress_iterations(1000); ++i) {
                auto other = copy;
                other.add_context("thread");
                EXPECT_EQ(other.context().size(), 1U);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_TRUE(original.context().empty());
}
