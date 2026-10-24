#include <gtest/gtest.h>

#include "test_support.hpp"
#include <thread>

using stepwright::errc;
using stepwright::error;

TEST(adapters, make_task_wraps_a_callable) {
    int calls = 0;
    stepwright::cancel_token seen;
    auto task =
        stepwright::make_task([&](stepwright::completion done, stepwright::cancel_token token) {
            ++calls;
            seen = std::move(token);
            done(error::make(errc::closed));
        });
    stepwright::cancel_source source;
    test_support::receiver receiver;
    task.start(receiver.callback(), source.token());
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(seen.can_be_cancelled());
    EXPECT_TRUE(receiver.result.is(errc::closed));
    // Usable as a machine child.
    struct parent : stepwright::machine<parent> {
        stepwright::task* child = nullptr;
        step begin() { return forward(*child); }
    } op;
    op.child = &task;
    test_support::receiver parent_receiver;
    op.start(parent_receiver.callback());
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(parent_receiver.result.is(errc::closed));
}

TEST(adapters, finally_runs_cleanup_once_with_the_result) {
    test_support::async_child inner;
    struct cleanup_state {
        int calls = 0;
        error seen;
    } state;
    stepwright::finally_task task(
        inner,
        [](void* ctx, const error& result) noexcept {
            auto& s = *static_cast<cleanup_state*>(ctx);
            ++s.calls;
            s.seen = result;
        },
        &state);
    stepwright::cancel_source source;
    test_support::receiver receiver;
    task.start(receiver.callback(), source.token());
    EXPECT_TRUE(inner.received_token.can_be_cancelled());
    EXPECT_EQ(state.calls, 0);
    inner.finish(error::make(errc::timed_out));
    EXPECT_EQ(state.calls, 1);
    EXPECT_TRUE(state.seen.is(errc::timed_out));
    EXPECT_TRUE(receiver.result.is(errc::timed_out));
}

TEST(adapters, finally_with_callable_reference_and_sync_inner) {
    test_support::sync_child inner;
    int calls = 0;
    bool cleaned_before_completion = false;
    auto cleanup = [&](const error& result) noexcept {
        ++calls;
        EXPECT_FALSE(result);
    };
    stepwright::finally_task task(inner, cleanup);
    struct order_receiver {
        int* calls;
        bool* ordered;
        void done(const error&) noexcept { *ordered = *calls == 1; }
    } r{.calls = &calls, .ordered = &cleaned_before_completion};
    task.start(stepwright::completion::bind<&order_receiver::done>(&r));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(cleaned_before_completion);
    task.start(stepwright::completion::bind<&order_receiver::done>(&r));
    EXPECT_EQ(calls, 2);
}

TEST(adapters, reschedule_runs_on_the_executor) {
    stepwright::manual_executor ex;
    stepwright::reschedule hop(ex);
    std::thread::id completed_on;
    struct thread_receiver {
        std::thread::id* where;
        int calls = 0;
        void done(const error& e) noexcept {
            EXPECT_FALSE(e);
            *where = std::this_thread::get_id();
            ++calls;
        }
    } r{.where = &completed_on};
    hop.start(stepwright::completion::bind<&thread_receiver::done>(&r));
    EXPECT_EQ(r.calls, 0);
    EXPECT_EQ(ex.pending(), 1U);
    std::thread runner([&] { ex.run_all(); });
    const auto runner_id = runner.get_id();
    runner.join();
    EXPECT_EQ(r.calls, 1);
    EXPECT_EQ(completed_on, runner_id);
    stepwright::inline_executor inline_ex;
    stepwright::reschedule now(inline_ex);
    now.start(stepwright::completion::bind<&thread_receiver::done>(&r));
    EXPECT_EQ(r.calls, 2);
}

TEST(adapters, machine_hops_with_reschedule) {
    stepwright::manual_executor ex;
    struct op_type : stepwright::machine<op_type> {
        explicit op_type(stepwright::executor& e) : hop(e) {}
        stepwright::reschedule hop;
        bool ran_on_executor = false;
        stepwright::manual_executor* ex = nullptr;
        step begin() { return after<&op_type::hopped>(hop); }
        step hopped() {
            ran_on_executor = ex->running_in_this_thread();
            return done();
        }
    } op(ex);
    op.ex = &ex;
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    ex.run_all();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_TRUE(op.ran_on_executor);
}
