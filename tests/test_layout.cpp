#include <gtest/gtest.h>

#include "test_layout_sample.hpp"
#include <iostream>

static_assert(STEPWRIGHT_DEBUG == 1);

TEST(machine, regression_layout_ndebug) {
    EXPECT_EQ(sizeof(layout_sample), ndebug_size());
    EXPECT_EQ(alignof(layout_sample), ndebug_align());
#if !STEPWRIGHT_TRACK_TASKS
    EXPECT_LE(sizeof(layout_sample), 112U); // tracking adds the registry node
#endif
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
TEST(layout, regression_m2_layout_ndebug) {
    const auto debug = measure_m2_layout();
    const auto release = ndebug_m2_layout();
    const std::array<const char*, m2_layout_count> names{
        STEPWRIGHT_M2_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_NAME)};
    for (std::size_t i = 0; i < m2_layout_count; ++i) {
        EXPECT_EQ(debug[i], release[i]) << names[i];
        std::cout << names[i] << " size=" << debug[i].first << " align=" << debug[i].second << '\n';
    }
}
TEST(layout, regression_m3_layout_ndebug) {
    const auto debug = measure_m3_layout();
    const auto release = ndebug_m3_layout();
    const std::array<const char*, m3_layout_count> names{
        STEPWRIGHT_M3_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_NAME)};
    for (std::size_t i = 0; i < m3_layout_count; ++i) {
        EXPECT_EQ(debug[i], release[i]) << names[i];
        std::cout << names[i] << " size=" << debug[i].first << " align=" << debug[i].second << '\n';
    }
}
TEST(layout, regression_m4_layout_ndebug) {
    const auto debug = measure_m4_layout();
    const auto release = ndebug_m4_layout();
    const std::array<const char*, m4_layout_count> names{
        STEPWRIGHT_M4_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_NAME)};
    for (std::size_t i = 0; i < m4_layout_count; ++i) {
        EXPECT_EQ(debug[i], release[i]) << names[i];
        std::cout << names[i] << " size=" << debug[i].first << " align=" << debug[i].second << '\n';
    }
}
