#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "test_support.hpp"
#include <array>
#include <deque>

using namespace std::chrono_literals;

TEST(alloc_usability, sync_wait_and_virtual_run_allocate_nothing) {
    test_support::sync_child child;
    stepwright::cancel_source source;
    stepwright::testing::virtual_executor ex;
    bool failed = false;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        EXPECT_EQ(count_allocations([&] {
                      for (int i = 0; i < 100; ++i) {
                          failed = failed || static_cast<bool>(stepwright::sync_wait(child, token));
                      }
                  }),
                  0U);
        EXPECT_EQ(count_allocations([&] {
                      for (int i = 0; i < 100; ++i) {
                          failed = failed || static_cast<bool>(ex.run(child, token));
                      }
                  }),
                  0U);
    }
    EXPECT_FALSE(failed);
}

TEST(alloc_usability, containers_copy_addresses_once_runs_allocate_nothing) {
    std::deque<test_support::sync_child> children(4);
    EXPECT_EQ(count_allocations([&] { stepwright::when_all_range all(children); }), 2U);
    EXPECT_EQ(count_allocations([&] { stepwright::when_any_range any(children); }), 3U);
    EXPECT_EQ(count_allocations([&] { stepwright::sequence_range sequence(children); }), 1U);
    std::vector<test_support::sync_child> large(100);
    EXPECT_EQ(count_allocations([&] { stepwright::when_all_range all(large); }), 3U);
    std::array<stepwright::task*, 1> pointers{&children[0]};
    EXPECT_EQ(count_allocations([&] { stepwright::sequence_range sequence(pointers); }), 0U);
    stepwright::when_all_range all(children);
    stepwright::when_any_range any(children);
    stepwright::sequence_range sequence(children);
    stepwright::sequence fixed(children[0], children[1]);
    stepwright::cancel_source source;
    bool failed = false;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        EXPECT_EQ(count_allocations([&] {
                      for (stepwright::task* t :
                           std::array<stepwright::task*, 4>{&all, &any, &sequence, &fixed}) {
                          failed = failed || static_cast<bool>(stepwright::sync_wait(*t, token));
                      }
                  }),
                  0U);
    }
    EXPECT_FALSE(failed);
}

TEST(alloc_usability, retry_including_failed_attempts_and_reusable_ops) {
    stepwright::testing::virtual_executor ex;
    const auto failure = stepwright::error::make(std::errc::io_error);
    int calls = 0;
    auto child =
        stepwright::make_task([&](stepwright::completion done, const stepwright::cancel_token&) {
            const auto result = ++calls % 3 == 0 ? stepwright::error{} : failure;
            done(result);
        });
    stepwright::retry retry(child, ex, {.max_attempts = 3, .initial_backoff = 1s});
    stepwright::cancel_source source;
    bool failed = false;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        EXPECT_EQ(
            count_allocations([&] { failed = failed || static_cast<bool>(ex.run(retry, token)); }),
            0U);
    }
    stepwright::channel<int> queue(1);
    stepwright::channel<int>::send_op send(queue);
    stepwright::broadcast<int> broadcast(1);
    auto sub = broadcast.subscribe();
    stepwright::broadcast<int>::publish_op publish(broadcast);
    stepwright::delay wait(ex, 1s);
    stepwright::with_deadline deadline(wait, ex, 10s);
    // Warm up the deadline's existing lazy cancellation source, not a new cost.
    EXPECT_FALSE(ex.run(deadline));
    int sum = 0;
    EXPECT_EQ(count_allocations([&] {
                  for (int i = 0; i < 10; ++i) {
                      failed = failed || static_cast<bool>(stepwright::sync_wait(send.with(i)));
                      sum += queue.try_pull().value();
                      failed = failed || static_cast<bool>(stepwright::sync_wait(publish.with(i)));
                      sum += sub.try_pull().value();
                      failed = failed || static_cast<bool>(ex.run(deadline.with(20s)));
                  }
              }),
              0U);
    EXPECT_EQ(sum, 90);
    EXPECT_FALSE(failed);
}
