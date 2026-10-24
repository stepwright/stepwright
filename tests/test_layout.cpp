#include <gtest/gtest.h>

#include "test_layout_sample.hpp"
#include <iostream>

static_assert(STEPWRIGHT_DEBUG == 1);

TEST(machine, regression_layout_ndebug) {
    EXPECT_EQ(sizeof(layout_sample), ndebug_size());
    EXPECT_EQ(alignof(layout_sample), ndebug_align());
    EXPECT_LE(sizeof(layout_sample), 112U);
    std::cout << "machine=" << sizeof(layout_sample) << " align=" << alignof(layout_sample)
              << " work_item=" << sizeof(stepwright::work_item)
              << " registration=" << sizeof(stepwright::cancel_registration)
              << " token=" << sizeof(stepwright::cancel_token)
              << " error=" << sizeof(stepwright::error)
              << " completion=" << sizeof(stepwright::completion)
              << " source=" << sizeof(stepwright::cancel_source)
              << " task=" << sizeof(stepwright::task) << " step=" << sizeof(stepwright::step)
              << " manual_executor=" << sizeof(stepwright::manual_executor) << '\n';
}
