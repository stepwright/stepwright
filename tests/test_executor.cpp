#include <stepwright/executor.hpp>

#include <gtest/gtest.h>

#include "stress.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <vector>

TEST(executor, fifo_drain_and_scope) {
    stepwright::manual_executor ex;
    stepwright::manual_executor nested;
    std::vector<int> order;
    EXPECT_FALSE(ex.running_in_this_thread());
    ex.post([&] {
        EXPECT_TRUE(ex.running_in_this_thread());
        order.push_back(1);
        ex.post([&] { order.push_back(3); });
        nested.post([&] {
            EXPECT_TRUE(ex.running_in_this_thread());
            EXPECT_TRUE(nested.running_in_this_thread());
        });
        EXPECT_TRUE(nested.run_one());
        EXPECT_FALSE(nested.running_in_this_thread());
    });
    ex.post([&] { order.push_back(2); });
    EXPECT_EQ(ex.pending(), 2U);
    EXPECT_EQ(ex.run_all(), 3U);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_FALSE(ex.run_one());
    EXPECT_EQ(ex.pending(), 0U);
    EXPECT_FALSE(ex.running_in_this_thread());
}
TEST(executor, concurrent_post_eight_threads) {
    struct item : stepwright::work_item {
        std::atomic<std::size_t>* count = nullptr;
    };
    stepwright::manual_executor ex;
    std::atomic<std::size_t> calls{0};
    const auto per_thread = static_cast<std::size_t>(test_support::stress_iterations(256));
    std::array<std::vector<item>, 8> items;
    std::array<std::thread, 8> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        items[i].resize(per_thread);
        threads[i] = std::thread([&, i] {
            EXPECT_FALSE(ex.running_in_this_thread());
            for (auto& node : items[i]) {
                node.count = &calls;
                node.run = [](stepwright::work_item* self) noexcept {
                    static_cast<item*>(self)->count->fetch_add(1, std::memory_order_relaxed);
                };
                ex.post(node);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(ex.run_all(), per_thread * threads.size());
    EXPECT_EQ(calls.load(), per_thread * threads.size());
}
TEST(executor, inline_and_base) {
    stepwright::inline_executor ex;
    EXPECT_TRUE(ex.running_in_this_thread());
    bool ran = false;
    ex.post([&] { ran = true; });
    EXPECT_TRUE(ran);
    struct custom : stepwright::executor {
        void post(stepwright::work_item& item) noexcept override { item.run(&item); }
    } base;
    EXPECT_FALSE(base.running_in_this_thread());
}
