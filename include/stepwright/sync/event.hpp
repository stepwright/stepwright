#pragma once

#include <stepwright/detail/async_op.hpp>
#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/detail/wait_list.hpp>

#include <atomic>
#include <mutex>

namespace stepwright {
/// One-shot signal awaited by many tasks. set() publishes a result once; waiters
/// registered before set() complete with it in FIFO order through the hand-off
/// trampoline, waiters started afterwards complete synchronously. All members are
/// thread-safe except reset(). The event must outlive every waiter bound to it;
/// Debug asserts on destruction with registered waiters. No allocation per wait.
class event {
public:
    class waiter;

    /// Construct an unset event.
    event() noexcept = default;
    event(const event&) = delete;
    event& operator=(const event&) = delete;
    /// Destroy only without registered waiters (Debug asserts).
    ~event() { STEPWRIGHT_ASSERT(waiters_.empty(), "destroying an event with registered waiters"); }

    /// Publish result (empty means success) and release every registered waiter.
    /// The first call wins; later calls are ignored (Debug asserts on them). The
    /// waiters' completions run on this thread, possibly after set() returns when
    /// called from inside another trampoline-delivered completion. set() does not
    /// access the event after its waiters have been detached.
    void set(error result = {}) noexcept;
    /// Thread-safe query; true once set() has been called (until reset()).
    bool is_set() const noexcept { return set_.load(std::memory_order_acquire); }
    /// Return to the unset state. Precondition: no registered waiters, and not
    /// concurrent with set() or a waiter start.
    void reset() noexcept {
        const std::lock_guard lock(lock_);
        STEPWRIGHT_ASSERT(waiters_.empty(), "event reset with registered waiters");
        set_.store(false, std::memory_order_relaxed);
        result_ = {};
    }
    /// Return a waiter task bound to this event (guaranteed copy elision).
    waiter wait() noexcept;

private:
    detail::spin_lock lock_;
    std::atomic<bool> set_{false};
    error result_;
    detail::wait_list waiters_;
};

/// Task completing with the event's result: synchronously when started after
/// set(), otherwise when set() is called, or with errc::cancelled when its token
/// is cancelled first (it unregisters in O(1)). Exactly one outcome per start.
/// Restartable after completion; start only while idle. Must outlive its
/// completion (Debug asserts on destruction while started); the event must
/// outlive the waiter's use of it.
class event::waiter final : public detail::async_op, private detail::wait_link {
public:
    /// Bind to ev without registering; registration happens at start.
    explicit waiter(event& ev) noexcept : event_(&ev) {}
    waiter(const waiter&) = delete;
    waiter& operator=(const waiter&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~waiter() override = default;

private:
    friend class event;
    static waiter& from(detail::wait_link* link) noexcept { return static_cast<waiter&>(*link); }

    void on_start(completion done, cancel_token token) override {
        event& ev = *event_;
        if (!ev.set_.load(std::memory_order_acquire)) {
            std::unique_lock lock(ev.lock_);
            if (!ev.set_.load(std::memory_order_relaxed)) {
                prepare(done, token);
                ev.waiters_.push_back(*this);
                lock.unlock();
                if (token.can_be_cancelled()) {
                    arm(token, &on_cancel);
                }
                return;
            }
        }
        const error result = ev.result_;
        done(result);
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<waiter*>(context);
        event& ev = *self.event_;
        bool removed = false;
        {
            const std::lock_guard lock(ev.lock_);
            if (detail::wait_list::linked(self)) {
                ev.waiters_.remove(self);
                removed = true;
            }
        }
        if (removed) {
            self.resolve(error::make(errc::cancelled));
        }
    }

    event* event_;
};

inline event::waiter event::wait() noexcept {
    return waiter(*this);
}

inline void event::set(error result) noexcept {
    detail::wait_link* chain = nullptr;
    error delivered;
    {
        const std::lock_guard lock(lock_);
        if (set_.load(std::memory_order_relaxed)) {
            STEPWRIGHT_ASSERT(false, "event set twice");
            return;
        }
        result_ = std::move(result);
        delivered = result_;
        set_.store(true, std::memory_order_release);
        chain = waiters_.take_all();
    }
    // The detached chain is private to this call; cancellation callbacks see the
    // nodes as unlinked and leave them alone. Do not touch *this from here on.
    const detail::handoff_scope batch;
    while (chain) {
        detail::wait_link* next = chain->next;
        waiter::from(chain).resolve(delivered);
        chain = next;
    }
}
} // namespace stepwright
