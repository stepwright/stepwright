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
TEST(layout, regression_primitive_layout_ndebug) {
    const auto debug = measure_primitive_layout();
    const auto release = ndebug_primitive_layout();
    const std::array<const char*, primitive_layout_count> names{
        STEPWRIGHT_PRIMITIVE_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_NAME)};
    for (std::size_t i = 0; i < primitive_layout_count; ++i) {
        EXPECT_EQ(debug[i], release[i]) << names[i];
        std::cout << names[i] << " size=" << debug[i].first << " align=" << debug[i].second << '\n';
    }
}
TEST(layout, regression_stream_layout_ndebug) {
    const auto debug = measure_stream_layout();
    const auto release = ndebug_stream_layout();
    const std::array<const char*, stream_layout_count> names{
        STEPWRIGHT_STREAM_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_NAME)};
    for (std::size_t i = 0; i < stream_layout_count; ++i) {
        EXPECT_EQ(debug[i], release[i]) << names[i];
        std::cout << names[i] << " size=" << debug[i].first << " align=" << debug[i].second << '\n';
    }
}
TEST(layout, regression_runtime_layout_ndebug) {
    const auto debug = measure_runtime_layout();
    const auto release = ndebug_runtime_layout();
    const std::array<const char*, runtime_layout_count> names{
        STEPWRIGHT_RUNTIME_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_NAME)};
    for (std::size_t i = 0; i < runtime_layout_count; ++i) {
        EXPECT_EQ(debug[i], release[i]) << names[i];
        std::cout << names[i] << " size=" << debug[i].first << " align=" << debug[i].second << '\n';
    }
}
