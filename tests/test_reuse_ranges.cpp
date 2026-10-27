#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <deque>
#include <forward_list>
#include <optional>
#include <type_traits>

using namespace std::chrono_literals;
namespace {
using child = test_support::sync_child;
template <class Join>
constexpr bool accepts_stable_ranges = std::is_constructible_v<Join, std::deque<child>&> &&
                                       std::is_constructible_v<Join, std::array<child, 2>&> &&
                                       std::is_constructible_v<Join, std::vector<child>&> &&
                                       std::is_constructible_v<Join, std::forward_list<child>&> &&
                                       !std::is_constructible_v<Join, std::deque<child>&&> &&
                                       !std::is_constructible_v<Join, const std::deque<child>&>;
static_assert(accepts_stable_ranges<stepwright::when_all_range>);
static_assert(accepts_stable_ranges<stepwright::when_any_range>);
static_assert(accepts_stable_ranges<stepwright::sequence_range>);
struct nonassignable {
    const int value;
    explicit nonassignable(int n) noexcept : value(n) {}
};
static_assert(!std::is_constructible_v<stepwright::channel<nonassignable>::send_op,
                                       stepwright::channel<nonassignable>&>);
static_assert(!std::is_constructible_v<stepwright::broadcast<nonassignable>::publish_op,
                                       stepwright::broadcast<nonassignable>&>);
} // namespace

TEST(container_joins, deque_array_vector_forward_range_and_restart) {
    std::deque<child> deque(4);
    stepwright::when_all_range all(deque, stepwright::when_all_policy::run_all);
    EXPECT_FALSE(stepwright::sync_wait(all));
    EXPECT_FALSE(stepwright::sync_wait(all));
    for (const auto& c : deque) {
        EXPECT_EQ(c.calls, 2);
    }
    std::array<child, 2> array;
    stepwright::when_any_range any(array);
    EXPECT_FALSE(stepwright::sync_wait(any));
    EXPECT_EQ(any.winner(), 0U);
    EXPECT_FALSE(stepwright::sync_wait(any));
    for (const auto& c : array) {
        EXPECT_EQ(c.calls, 2);
    }
    std::vector<child> vector(100); // also exercise the existing sharding threshold
    stepwright::when_all_range large(vector);
    EXPECT_FALSE(stepwright::sync_wait(large));
    for (const auto& c : vector) {
        EXPECT_EQ(c.calls, 1);
    }
    std::forward_list<child> list(3);
    stepwright::sequence_range sequence(list);
    EXPECT_FALSE(stepwright::sync_wait(sequence));
    for (const auto& c : list) {
        EXPECT_EQ(c.calls, 1);
    }
}

TEST(container_joins, copies_view_addresses_but_pointer_span_stays_borrowed) {
    std::array<child, 2> children;
    std::optional<stepwright::when_all_range> copied;
    {
        std::span<child> view(children);
        copied.emplace(view); // view storage itself need not survive construction
    }
    EXPECT_FALSE(stepwright::sync_wait(*copied));
    std::array<stepwright::task*, 1> pointers{children.data()};
    stepwright::when_all_range borrowed(pointers);
    pointers[0] = &children[1];
    EXPECT_FALSE(stepwright::sync_wait(borrowed));
    EXPECT_EQ(children[0].calls, 1);
    EXPECT_EQ(children[1].calls, 2);
    std::array<child, 0> none;
    stepwright::when_all_range empty(none);
    stepwright::sequence_range empty_sequence(none);
    EXPECT_FALSE(stepwright::sync_wait(empty));
    EXPECT_FALSE(stepwright::sync_wait(empty_sequence));
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::when_any_range invalid(none);
            stepwright::sync_wait(invalid);
        },
        "at least one child");
#endif
}

TEST(container_joins, cancellation_and_winner_semantics_are_unchanged) {
    std::deque<test_support::cancellable_child> children(3);
    stepwright::when_all_range all(children);
    test_support::receiver receiver;
    all.start(receiver.callback());
    children[1].complete(stepwright::error::make(stepwright::errc::closed));
    EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
    for (const auto& c : children) {
        EXPECT_FALSE(c.pending.load());
    }
    stepwright::when_any_range any(children);
    test_support::receiver race;
    any.start(race.callback());
    children[2].complete();
    EXPECT_FALSE(race.result);
    EXPECT_EQ(any.winner(), 2U);
    for (const auto& c : children) {
        EXPECT_FALSE(c.pending.load());
    }
}

TEST(reusable, sends_and_publications_support_nonassignable_and_rearm) {
    stepwright::channel<int> queue(1);
    stepwright::channel<int>::send_op send(queue);
    EXPECT_EQ(&send.with(4), &send);
    EXPECT_FALSE(stepwright::sync_wait(send));
    EXPECT_EQ(queue.try_pull().value(), 4);
    EXPECT_FALSE(stepwright::sync_wait(send.with(5)));
    EXPECT_EQ(queue.try_pull().value(), 5);
    stepwright::channel<nonassignable> fixed(1);
    stepwright::channel<nonassignable>::send_op nonassign(fixed, nonassignable(1));
    EXPECT_FALSE(stepwright::sync_wait(nonassign.with(nonassignable(7))));
    EXPECT_EQ(fixed.try_pull().value().value, 7);
    stepwright::broadcast<nonassignable> broadcast(1);
    auto sub = broadcast.subscribe();
    stepwright::broadcast<nonassignable>::publish_op publish(broadcast, nonassignable(1));
    EXPECT_EQ(&publish.with(nonassignable(8)), &publish);
    EXPECT_FALSE(stepwright::sync_wait(publish));
    EXPECT_EQ(sub.try_pull().value().value, 8);
    EXPECT_FALSE(stepwright::sync_wait(publish.with(nonassignable(9))));
    EXPECT_EQ(sub.try_pull().value().value, 9);
    stepwright::broadcast<int> ints(1);
    stepwright::broadcast<int>::publish_op default_publish(ints);
    EXPECT_FALSE(stepwright::sync_wait(default_publish.with(3)));
}

TEST(reusable, cancellation_retains_value_then_allows_replacement) {
    stepwright::channel<int> queue(1);
    queue.try_send(1);
    stepwright::channel<int>::send_op send(queue);
    stepwright::cancel_source stop;
    stop.request_cancel();
    EXPECT_TRUE(stepwright::sync_wait(send.with(2), stop.token()).is(stepwright::errc::cancelled));
    EXPECT_EQ(send.value(), 2);
    EXPECT_EQ(queue.try_pull().value(), 1);
    EXPECT_FALSE(stepwright::sync_wait(send.with(3)));
    EXPECT_EQ(queue.try_pull().value(), 3);
    stepwright::broadcast<int> broadcast(1);
    auto sub = broadcast.subscribe();
    broadcast.try_publish(1);
    stepwright::broadcast<int>::publish_op publish(broadcast);
    EXPECT_TRUE(
        stepwright::sync_wait(publish.with(2), stop.token()).is(stepwright::errc::cancelled));
    EXPECT_EQ(publish.value(), 2);
    EXPECT_EQ(sub.try_pull().value(), 1);
    EXPECT_FALSE(stepwright::sync_wait(publish.with(3)));
    EXPECT_EQ(sub.try_pull().value(), 3);
}

TEST(reusable, delays_and_deadline_timeouts_change_only_the_next_idle_run) {
    stepwright::testing::virtual_executor ex;
    stepwright::delay wait(ex, 1s);
    EXPECT_EQ(&wait.with(2s), &wait);
    EXPECT_FALSE(ex.run(wait));
    EXPECT_EQ(ex.now().time_since_epoch(), 2s);
    EXPECT_FALSE(ex.run(wait.with(3s)));
    EXPECT_EQ(ex.now().time_since_epoch(), 5s);
    stepwright::with_deadline limited(wait.with(5s), ex, 10s);
    EXPECT_FALSE(ex.run(limited));
    EXPECT_EQ(&limited.with(1s), &limited);
    EXPECT_TRUE(ex.run(limited).is(stepwright::errc::timed_out));
    EXPECT_FALSE(ex.run(limited.with(10s)));
}

TEST(reusable, receiver_can_rearm_the_same_operation) {
    stepwright::channel<int> queue(2);
    stepwright::channel<int>::send_op send(queue);
    test_support::receiver final;
    auto restart = [&](const stepwright::error& e) {
        EXPECT_FALSE(e);
        send.with(2).start(final.callback());
    };
    send.with(1).start(stepwright::completion::to(restart));
    EXPECT_EQ(final.calls, 1);
    EXPECT_EQ(queue.try_pull().value(), 1);
    EXPECT_EQ(queue.try_pull().value(), 2);
}

TEST(reusable, debug_active_setters_are_rejected) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::channel<int> queue(1);
            queue.try_send(1);
            stepwright::channel<int>::send_op send(queue);
            test_support::receiver receiver;
            send.start(receiver.callback());
            send.with(2);
        },
        "send_op::with while active");
    EXPECT_DEATH(
        {
            stepwright::broadcast<int> broadcast(1);
            auto sub = broadcast.subscribe();
            broadcast.try_publish(1);
            stepwright::broadcast<int>::publish_op publish(broadcast);
            test_support::receiver receiver;
            publish.start(receiver.callback());
            publish.with(2);
        },
        "publish_op::with while active");
    EXPECT_DEATH(
        {
            stepwright::testing::virtual_executor ex;
            stepwright::delay delay(ex, 1s);
            test_support::receiver receiver;
            delay.start(receiver.callback());
            delay.with(2s);
        },
        "delay::with while active");
    EXPECT_DEATH(
        {
            stepwright::testing::virtual_executor ex;
            stepwright::delay delay(ex, 1s);
            stepwright::with_deadline limit(delay, ex, 2s);
            test_support::receiver receiver;
            limit.start(receiver.callback());
            limit.with(3s);
        },
        "with_deadline::with while active");
#endif
}
