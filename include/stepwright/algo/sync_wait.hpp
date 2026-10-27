#pragma once

#include <stepwright/detail/handoff.hpp>
#include <stepwright/task.hpp>
#include <stepwright/thread_pool.hpp>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace stepwright {
/// Start t and block this thread until completion, returning its result. The
/// waiter lives on the stack; successful calls allocate nothing. t must be idle
/// and survive completion. Do not call from steps, pool workers or hand-off
/// callbacks (Debug diagnoses the latter two), or when this thread must drive t.
/// Use testing::virtual_executor::run for caller-driven work instead.
inline error sync_wait(task& t, cancel_token token = {}) {
    STEPWRIGHT_ASSERT(!thread_pool::running_on_worker_thread(), "sync_wait on a pool worker");
    STEPWRIGHT_ASSERT(!detail::handoff_state().active, "sync_wait inside a hand-off delivery");
    struct waiter {
        std::mutex mutex;
        std::condition_variable changed;
        error result;
        bool done = false;

        void finish(const error& e) noexcept {
            const std::lock_guard lock(mutex);
            result = e;
            done = true;
            // Unlock is our last access. The waiter cannot return and destroy
            // changed until it reacquires mutex after this notification.
            changed.notify_one();
        }
    } state;
    t.start(completion::bind<&waiter::finish>(&state), std::move(token));
    std::unique_lock lock(state.mutex);
    state.changed.wait(lock, [&] { return state.done; });
    return state.result;
}
} // namespace stepwright
