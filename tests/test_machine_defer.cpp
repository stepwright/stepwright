#include <gtest/gtest.h>

#include "test_support.hpp"
#include <atomic>
#include <thread>
#include <vector>

TEST(machine, defer_order_and_then) {
    struct deferred : stepwright::machine<deferred> {
        std::vector<int> order;
        step begin() {
            order.push_back(1);
            return defer<&deferred::second>();
        }
        step second(const stepwright::error& e) {
            EXPECT_FALSE(e);
            EXPECT_TRUE(home()->running_in_this_thread());
            order.push_back(3);
            return then<&deferred::third>();
        }
        step third() {
            order.push_back(4);
            return done();
        }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 0);
    op.order.push_back(2);
    EXPECT_EQ(ex.run_all(), 1U);
    EXPECT_EQ(op.order, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_EQ(receiver.calls, 1);
}
TEST(machine, regression_inline_defer_stack) {
    struct deferred : stepwright::machine<deferred> {
        int remaining = test_support::stress_iterations(100'000);
        step begin() { return remaining-- == 0 ? done() : defer<&deferred::begin>(); }
    } op;
    stepwright::inline_executor ex;
    op.bind(ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    EXPECT_EQ(receiver.calls, 1);
}
TEST(machine, regression_early_wake_survives_defer) {
    struct deferred : stepwright::machine<deferred> {
        bool early = false;
        step begin() {
            if (early) {
                wake(stepwright::error::make(stepwright::errc::closed));
            }
            return defer<&deferred::next_step>();
        }
        step next_step(const stepwright::error& e) {
            EXPECT_FALSE(e); // Defer never consumes the upcoming park's error.
            return park<&deferred::finish>();
        }
        step finish(const stepwright::error& e) { return fail(e); }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    for (bool early : {false, true}) {
        op.early = early;
        test_support::receiver receiver;
        op.start(receiver.callback());
        if (!early) {
            op.wake(stepwright::error::make(stepwright::errc::closed));
        }
        EXPECT_EQ(receiver.calls, 0);
        EXPECT_EQ(ex.pending(), 1U);
        EXPECT_EQ(ex.run_all(), 1U);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
        EXPECT_EQ(receiver.calls, 1);
    }
}

namespace {
// A multi-threaded executor at its fastest: post() hands the item to a worker
// thread and returns only after it has run there, i.e. always before the posting
// step has returned to its driver.
class eager_thread_executor final : public stepwright::executor {
public:
    eager_thread_executor()
        : thread_([this] {
            for (;;) {
                stepwright::work_item* item = nullptr;
                while ((item = pending_.exchange(nullptr)) == nullptr) {
                    if (stop_.load()) {
                        return;
                    }
                    std::this_thread::yield();
                }
                inside = true;
                item->run(item);
                inside = false;
                done_.store(true);
            }
        }) {}
    eager_thread_executor(const eager_thread_executor&) = delete;
    eager_thread_executor& operator=(const eager_thread_executor&) = delete;
    ~eager_thread_executor() override {
        stop_.store(true);
        thread_.join();
    }
    void post(stepwright::work_item& item) noexcept override {
        if (running_in_this_thread()) {
            item.run(&item); // the worker posting to itself runs inline
            return;
        }
        done_.store(false);
        pending_.store(&item);
        while (!done_.load()) {
            std::this_thread::yield();
        }
    }
    bool running_in_this_thread() const noexcept override { return inside; }
    std::thread::id id() const noexcept { return thread_.get_id(); }

private:
    inline static thread_local bool inside = false;
    std::atomic<stepwright::work_item*> pending_{nullptr};
    std::atomic<bool> done_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
} // namespace

TEST(machine, regression_defer_runs_on_the_executor_when_it_runs_first) {
    // Deferring from a foreign thread used to post while still owning the machine:
    // when the executor ran the item before the driver suspended, the item only
    // marked the step ready and the step continued on the foreign thread.
    struct deferred : stepwright::machine<deferred> {
        eager_thread_executor* ex = nullptr;
        int on_executor = 0;
        int remaining = 0;
        step begin() { return defer<&deferred::hop>(); }
        step hop() {
            on_executor += ex->running_in_this_thread() ? 1 : 0;
            // From the executor thread the next deferral runs inline (flat loop).
            return --remaining == 0 ? done() : defer<&deferred::hop>();
        }
    } op;
    eager_thread_executor ex;
    op.ex = &ex;
    op.bind(ex);
    for (int round = 0; round < 100; ++round) {
        op.remaining = 10;
        op.on_executor = 0;
        test_support::receiver receiver;
        op.start(receiver.callback()); // begin() runs here, on the test thread
        receiver.wait();
        EXPECT_EQ(op.on_executor, 10);
    }
}
