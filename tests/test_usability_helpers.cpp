#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <memory>
#include <type_traits>

using namespace std::chrono_literals;
namespace {
template <class F>
concept temporary_completion = requires(F&& f) { stepwright::completion::to(std::move(f)); };
struct callable {
    int* calls;
    void operator()(const stepwright::error&) const noexcept { ++*calls; }
};
static_assert(!temporary_completion<callable>);
static_assert(!temporary_completion<const callable>);
static_assert(std::is_trivially_copyable_v<stepwright::completion>);

template <class Code>
struct failed_machine : stepwright::machine<failed_machine<Code>> {
    Code code;
    explicit failed_machine(Code c) : code(c) {}
    stepwright::step begin() { return this->fail(code, "bad input"); }
};
template <class Code>
struct failed_source : stepwright::source_machine<failed_source<Code>, int> {
    Code code;
    explicit failed_source(Code c) : code(c) {}
    stepwright::source_step begin() { return this->fail(code, "bad input"); }
};
template <class Code>
void check_failure(Code code, std::error_code expected) {
    failed_machine<Code> op(code);
    auto result = stepwright::sync_wait(op);
    EXPECT_EQ(result.code(), expected);
    EXPECT_EQ(result.message(), "bad input");
    ASSERT_EQ(result.context().size(), 1U);
    failed_source<Code> source(code);
    auto item = source.try_pull();
    ASSERT_TRUE(item.is_failed());
    EXPECT_EQ(item.failure_reason().code(), expected);
    EXPECT_EQ(item.failure_reason().message(), "bad input");
    EXPECT_EQ(item.failure_reason().context().size(), 1U);
}
} // namespace

TEST(usability, callable_completion_borrows_mutable_const_and_pointer_callables) {
    int calls = 0;
    const callable const_fn{&calls};
    auto copy = stepwright::completion::to(const_fn);
    copy({});
    auto mutable_fn = [count = 0, &calls](const stepwright::error&) mutable { calls += ++count; };
    stepwright::completion::to(mutable_fn)({});
    stepwright::completion::to(mutable_fn)({});
    auto pointer = +[](const stepwright::error&) noexcept {};
    stepwright::completion::to(pointer)({});
    auto reference = std::ref(const_fn);
    stepwright::completion::to(reference)({});
    EXPECT_EQ(calls, 5);
}

TEST(usability, fail_code_message_keeps_context_and_empty_error_compatibility) {
    check_failure(stepwright::errc::closed, stepwright::make_error_code(stepwright::errc::closed));
    check_failure(std::errc::invalid_argument, std::make_error_code(std::errc::invalid_argument));
    check_failure(std::error_code(7, std::system_category()), {7, std::system_category()});
    failed_machine<std::error_code> zero({});
    EXPECT_FALSE(stepwright::sync_wait(zero));
    failed_source<std::error_code> end({});
    EXPECT_TRUE(end.try_pull().at_end());
    struct empty_machine : stepwright::machine<empty_machine> {
        step begin() { return fail({}); }
    } empty;
    struct empty_source : stepwright::source_machine<empty_source, int> {
        step begin() { return fail({}); }
    } source;
    EXPECT_FALSE(stepwright::sync_wait(empty));
    EXPECT_TRUE(source.try_pull().at_end());
    struct no_message : stepwright::machine<no_message> {
        step begin() { return fail(stepwright::errc::closed); }
    } no_text;
    EXPECT_TRUE(stepwright::sync_wait(no_text).message().empty());
}

TEST(sync_wait, synchronous_pool_timer_failure_and_cancellation) {
    test_support::sync_child synchronous;
    EXPECT_FALSE(stepwright::sync_wait(synchronous));
    synchronous.result = stepwright::error::make(stepwright::errc::closed, "closed");
    EXPECT_EQ(stepwright::sync_wait(synchronous).message(), "closed");
    stepwright::thread_pool pool(2);
    stepwright::reschedule hop(pool);
    EXPECT_FALSE(stepwright::sync_wait(hop));
    stepwright::steady_timer_service timers;
    stepwright::delay pause(timers, 1ms);
    EXPECT_FALSE(stepwright::sync_wait(pause));
    stepwright::cancel_source stop;
    stop.request_cancel();
    EXPECT_TRUE(
        stepwright::sync_wait(pause.with(1h), stop.token()).is(stepwright::errc::cancelled));
    pool.wait_idle();
}

TEST(sync_wait, foreign_cancellation_and_immediate_stack_teardown_stress) {
    stepwright::event event;
    auto wait = event.wait();
    stepwright::cancel_source stop;
    std::atomic<bool> entered{false};
    auto announce =
        stepwright::make_task([&](stepwright::completion done, stepwright::cancel_token token) {
            entered.store(true, std::memory_order_release);
            wait.start(done, std::move(token));
        });
    std::thread cancel([&] {
        while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        stop.request_cancel();
    });
    EXPECT_TRUE(stepwright::sync_wait(announce, stop.token()).is(stepwright::errc::cancelled));
    cancel.join();
    stepwright::thread_pool pool(2);
    for (int i = 0; i < test_support::stress_iterations(2'000); ++i) {
        // Both task and waiter storage die immediately, without joining the
        // completing thread first. This exposes notification-after-return races.
        auto hop = std::make_unique<stepwright::reschedule>(pool);
        const auto result = stepwright::sync_wait(*hop);
        hop.reset();
        ASSERT_FALSE(result);
    }
    pool.wait_idle();
}

TEST(sync_wait, debug_rejects_worker_and_handoff_delivery) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::thread_pool pool(1);
            test_support::sync_child child;
            pool.post([&] { static_cast<void>(stepwright::sync_wait(child)); });
            pool.wait_idle();
        },
        "sync_wait on a pool worker");
    EXPECT_DEATH(
        {
            stepwright::work_item item;
            item.run = [](stepwright::work_item*) noexcept {
                test_support::sync_child child;
                static_cast<void>(stepwright::sync_wait(child));
            };
            stepwright::detail::handoff(item);
        },
        "sync_wait inside a hand-off");
#endif
}

TEST(virtual_run, stops_at_target_and_leaves_unrelated_work) {
    stepwright::testing::virtual_executor ex;
    int calls = 0;
    stepwright::work_item unrelated{.run = [](stepwright::work_item*) noexcept {}};
    ex.post(unrelated);
    test_support::sync_child synchronous;
    EXPECT_FALSE(ex.run(synchronous));
    EXPECT_EQ(ex.pending(), 1U);
    ex.run_until_idle();
    stepwright::delay later(ex, 10s);
    auto on_later = [&](const stepwright::error& e) {
        EXPECT_FALSE(e);
        ++calls;
    };
    later.start(stepwright::completion::to(on_later));
    stepwright::delay target(ex, 1s);
    EXPECT_FALSE(ex.run(target));
    EXPECT_EQ(ex.now().time_since_epoch(), 1s);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(ex.scheduled_timers(), 1U);
    ex.run_until_quiescent();
    EXPECT_EQ(calls, 1);
    synchronous.result = stepwright::error::make(stepwright::errc::closed);
    EXPECT_TRUE(ex.run(synchronous).is(stepwright::errc::closed));
}

TEST(virtual_run, stall_retains_receiver_and_can_settle_then_restart) {
    stepwright::testing::virtual_executor ex;
    test_support::parked op;
    stepwright::cancel_source stop;
    const auto stalled = ex.run(op, stop.token());
    EXPECT_EQ(stalled.code(), std::errc::resource_deadlock_would_occur);
    EXPECT_EQ(stalled.message(), "task cannot complete: nothing left to run");
    stop.request_cancel();
    EXPECT_TRUE(ex.run(op).is(stepwright::errc::cancelled));
    EXPECT_TRUE(ex.run(op, stop.token()).is(stepwright::errc::cancelled));
    EXPECT_EQ(ex.run(op).code(), std::errc::resource_deadlock_would_occur);
    op.wake();
    EXPECT_FALSE(ex.run(op));
}

TEST(virtual_run, debug_rejects_unsettled_other_task_and_nested_run) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::testing::virtual_executor ex;
            test_support::parked active;
            test_support::sync_child other;
            static_cast<void>(ex.run(active));
            static_cast<void>(ex.run(other));
        },
        "unsettled task");
    EXPECT_DEATH(
        {
            stepwright::testing::virtual_executor ex;
            test_support::sync_child child;
            auto nested =
                stepwright::make_task([&](stepwright::completion, const stepwright::cancel_token&) {
                    static_cast<void>(ex.run(child));
                });
            static_cast<void>(ex.run(nested));
        },
        "nested virtual_executor::run");
    EXPECT_DEATH(
        {
            auto ex = std::make_unique<stepwright::testing::virtual_executor>();
            test_support::parked active;
            static_cast<void>(ex->run(active));
            ex.reset();
        },
        "unsettled run");
#endif
}

TEST(virtual_run, foreign_completion_after_stall_uses_persistent_receiver) {
    stepwright::testing::virtual_executor ex;
    test_support::async_child child;
    EXPECT_EQ(ex.run(child).code(), std::errc::resource_deadlock_would_occur);
    std::thread producer([&] { child.finish(); });
    producer.join();
    EXPECT_FALSE(ex.run(child));
}
