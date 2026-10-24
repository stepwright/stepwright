#pragma once

#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/timer.hpp>

#include <cstddef>
#include <mutex>
#include <thread>

namespace stepwright::testing {
/// Deterministic timer service for tests: time only moves when advance() or
/// advance_to() is called, and due callbacks run on the calling thread. All
/// members are thread-safe. Debug asserts on destruction with scheduled entries.
class manual_timer_service final : public timer_service {
public:
    /// Start at clock::time_point{} (or at start).
    manual_timer_service() noexcept = default;
    /// Start at an explicit time.
    explicit manual_timer_service(clock::time_point start) noexcept : now_(start) {}
    manual_timer_service(const manual_timer_service&) = delete;
    manual_timer_service& operator=(const manual_timer_service&) = delete;
    /// Destroy only without scheduled entries (Debug asserts).
    ~manual_timer_service() override {
        STEPWRIGHT_ASSERT(queue_.empty(),
                          "destroying a manual_timer_service with scheduled timers");
    }

    /// The manual current time.
    clock::time_point now() const noexcept override {
        const std::lock_guard lock(lock_);
        return now_;
    }
    /// See timer_service::schedule. A deadline at or before now() fires on the
    /// next advance (including advance(0)).
    void schedule(entry& e, clock::time_point deadline) noexcept override {
        const std::lock_guard lock(lock_);
        queue_.schedule(e, deadline);
    }
    /// See timer_service::cancel; yields while e's callback runs on another thread.
    bool cancel(entry& e) noexcept override {
        std::unique_lock lock(lock_);
        for (;;) {
            switch (queue_.status(e)) {
                case detail::timer_queue::cancel_result::removed:
                    queue_.remove(e);
                    return true;
                case detail::timer_queue::cancel_result::not_pending:
                    return false;
                case detail::timer_queue::cancel_result::firing_elsewhere:
                    lock.unlock();
                    std::this_thread::yield();
                    lock.lock();
                    break;
            }
        }
    }
    /// Advance time by d and fire every entry due by the new time on this thread,
    /// in deadline order (equal deadlines FIFO). While an entry fires, now()
    /// reports its deadline, so callbacks that reschedule relative to now() and
    /// fall due within the window also fire in this call.
    void advance(clock::duration d) {
        clock::time_point target;
        {
            const std::lock_guard lock(lock_);
            target = now_ + d;
        }
        advance_to(target);
    }
    /// Advance to t (no-op for times in the past) and fire due entries as advance().
    void advance_to(clock::time_point t) {
        std::unique_lock lock(lock_);
        for (;;) {
            detail::timer_queue::firing record;
            entry* due = queue_.pop_due(t, record);
            if (!due) {
                break;
            }
            if (due->deadline > now_) {
                now_ = due->deadline;
            }
            lock.unlock();
            due->fire(due);
            lock.lock();
            queue_.fired(record);
        }
        if (t > now_) {
            now_ = t;
        }
    }
    /// Number of scheduled (not yet fired) entries.
    std::size_t pending() const noexcept {
        const std::lock_guard lock(lock_);
        return queue_.size();
    }

private:
    mutable detail::spin_lock lock_;
    clock::time_point now_;
    detail::timer_queue queue_;
};
} // namespace stepwright::testing
