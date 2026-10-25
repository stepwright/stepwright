// Shared by the examples: let main() wait for tasks.
//
// stepwright never blocks inside a step. Only the thread that owns the program (here
// main) may wait for a result, and only outside any executor: `waiter` is a
// completion target that signals a condition variable, and `run_blocking` starts
// a task with it and sleeps until it fires. Never use them from a step or from a
// thread_pool worker.
#pragma once

#include <stepwright/stepwright.hpp>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace example {
class waiter {
public:
    waiter() = default;
    waiter(const waiter&) = delete;
    waiter& operator=(const waiter&) = delete;
    ~waiter() = default;

    // The completion to start a task with (once).
    stepwright::completion callback() noexcept {
        return stepwright::completion::bind<&waiter::finish>(this);
    }
    // Whether the task has completed (does not wait).
    bool ready() {
        const std::lock_guard lock(mutex_);
        return done_;
    }
    // Sleep until the task has completed; return its result.
    stepwright::error wait() {
        std::unique_lock lock(mutex_);
        done_changed_.wait(lock, [&] { return done_; });
        return result_;
    }

private:
    void finish(const stepwright::error& result) noexcept {
        // Notify under the lock: the waiting thread may destroy *this as soon as
        // it sees done_.
        const std::lock_guard lock(mutex_);
        result_ = result;
        done_ = true;
        done_changed_.notify_one();
    }
    std::mutex mutex_;
    std::condition_variable done_changed_;
    bool done_ = false;
    stepwright::error result_;
};

// Start op and wait for its result.
inline stepwright::error run_blocking(stepwright::task& op, stepwright::cancel_token token = {}) {
    waiter w;
    op.start(w.callback(), std::move(token));
    return w.wait();
}
} // namespace example
