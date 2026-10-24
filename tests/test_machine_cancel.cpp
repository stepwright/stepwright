#include <gtest/gtest.h>

#include "test_support.hpp"
#include <atomic>
#include <barrier>
#include <memory>
#include <thread>

TEST(machine_cancel, propagation_and_completed_noop) {
    struct parent : stepwright::machine<parent> {
        test_support::async_child child;
        step begin() { return forward(child); }
    } op;
    stepwright::cancel_source source;
    test_support::receiver receiver;
    op.start(receiver.callback(), source.token());
    EXPECT_TRUE(op.child.received_token.can_be_cancelled());
    source.request_cancel();
    EXPECT_TRUE(op.child.received_token.cancel_requested());
    EXPECT_TRUE(op.cancel_requested());
    op.child.finish();
    EXPECT_EQ(receiver.calls, 1);
    test_support::parked parked;
    stepwright::cancel_source completed_source;
    test_support::receiver completed;
    parked.start(completed.callback(), completed_source.token());
    parked.wake();
    completed_source.request_cancel();
    EXPECT_EQ(completed.calls, 1);
}
TEST(machine_cancel, parked_and_pre_cancelled) {
    for (bool before : {false, true}) {
        stepwright::cancel_source source;
        if (before) {
            source.request_cancel();
        }
        test_support::parked op;
        test_support::receiver receiver;
        op.start(receiver.callback(), source.token());
        if (!before) {
            source.request_cancel();
        }
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::cancelled));
    }
}
TEST(machine_cancel, custom_hook_and_self_destroy) {
    struct custom : stepwright::machine<custom> {
        std::atomic<int> cancellations{0};
        step begin() { return park<&custom::finish>(); }
        step finish(const stepwright::error& e) { return fail(e); }
        void on_cancel() noexcept {
            cancellations.fetch_add(1);
            wake_if_parked();
        }
    } op;
    stepwright::cancel_source source;
    test_support::receiver receiver;
    op.start(receiver.callback(), source.token());
    source.request_cancel();
    EXPECT_EQ(op.cancellations.load(), 1);
    EXPECT_FALSE(receiver.result);
    auto destroyed = std::make_unique<test_support::parked>();
    stepwright::cancel_source destroy_source;
    destroyed->start({.fn =
                          [](void* ctx, const stepwright::error&) noexcept {
                              static_cast<std::unique_ptr<test_support::parked>*>(ctx)->reset();
                          },
                      .ctx = &destroyed},
                     destroy_source.token());
    destroy_source.request_cancel();
    EXPECT_FALSE(destroyed);
}
TEST(machine_cancel, regression_cancel_completion_race) {
    struct parent : stepwright::machine<parent> {
        test_support::async_child child;
        std::atomic<int> cancellations{0};
        step begin() { return forward(child); }
        void on_cancel() noexcept { cancellations.fetch_add(1, std::memory_order_relaxed); }
    } op;
    for (int i = 0; i < test_support::stress_iterations(1000); ++i) {
        stepwright::cancel_source source;
        test_support::receiver receiver;
        op.start(receiver.callback(), source.token());
        std::thread cancel([&] { source.request_cancel(); });
        op.child.finish();
        cancel.join();
        EXPECT_EQ(receiver.calls, 1);
    }
}
TEST(machine_cancel, regression_cancel_wake_vs_child_wait) {
    struct child_type : stepwright::task {
        stepwright::completion receiver;
        bool finished = false;
        bool started = false;
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            receiver = done;
            started = true;
            finished = false;
        }
    };
    struct racing : stepwright::machine<racing> {
        child_type child;
        int premature = 0;
        int child_steps = 0;
        step begin() { return park<&racing::on_woken>(); }
        step on_woken(const stepwright::error& e) {
            return e ? fail(e) : after<&racing::on_child>(child);
        }
        step on_child(const stepwright::error& e) {
            ++child_steps;
            if (!child.finished) {
                ++premature;
            }
            return fail(e);
        }
    } op;
    std::barrier rendezvous(3);
    std::atomic<bool> stop{false};
    std::atomic<stepwright::cancel_source*> source{nullptr};
    std::atomic<int> wakes_finished{0};
    std::atomic<int> cancels_finished{0};
    std::thread waker([&] {
        for (;;) {
            rendezvous.arrive_and_wait();
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            // Cancellation may already have finished the operation. Unlike the
            // Release-only probe's wake(), this is legal even in that ordering.
            op.wake_if_parked();
            wakes_finished.fetch_add(1, std::memory_order_release);
            rendezvous.arrive_and_wait();
        }
    });
    std::thread canceller([&] {
        for (;;) {
            rendezvous.arrive_and_wait();
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            source.load(std::memory_order_acquire)->request_cancel();
            cancels_finished.fetch_add(1, std::memory_order_release);
            rendezvous.arrive_and_wait();
        }
    });
    int started_children = 0;
    const int iterations = test_support::stress_iterations(20'000);
    for (int i = 0; i < iterations; ++i) {
        stepwright::cancel_source current;
        source.store(&current, std::memory_order_release);
        op.child.started = false;
        test_support::receiver receiver;
        op.start(receiver.callback(), current.token());
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        // Explicit visibility also works with older libc++ barrier implementations
        // whose phase hand-off is not fully understood by macOS TSan.
        while (wakes_finished.load(std::memory_order_acquire) != i + 1 ||
               cancels_finished.load(std::memory_order_acquire) != i + 1) {
            std::this_thread::yield();
        }
        if (op.child.started) {
            ++started_children;
            EXPECT_FALSE(receiver.completed.load(std::memory_order_acquire));
            op.child.finished = true;
            const auto done = op.child.receiver;
            done({}); // Always deliver the real result, even if a bad wake won.
        }
        receiver.wait();
        EXPECT_EQ(receiver.calls, 1);
    }
    stop.store(true, std::memory_order_release);
    rendezvous.arrive_and_wait();
    waker.join();
    canceller.join();
    EXPECT_EQ(op.premature, 0);
    EXPECT_EQ(op.child_steps, started_children);
    EXPECT_GT(started_children, 0);
    RecordProperty("iterations", iterations);
    RecordProperty("premature_child_steps", op.premature);
}
