#pragma once

#include <stepwright/detail/async_op.hpp>
#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/detail/wait_list.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace stepwright {
/// Runs one inner task at most once per run and shares its result with every
/// waiter. The first started waiter starts the inner task (lazily) with an internal
/// cancel token that cancel() requests; waiters complete in FIFO order with the
/// inner result through the hand-off trampoline, and waiters started after the
/// inner completed complete synchronously. A waiter cancelled through its own
/// token completes with errc::cancelled; the inner task keeps running.
/// Thread-safe except reset(). Non-owning: the inner task and this object must
/// outlive the run and every waiter; Debug asserts on destruction while running
/// or with registered waiters. The first run allocates the internal cancel source.
class shared_task {
public:
    class waiter;

    /// Bind to inner (non-owning) without starting it.
    explicit shared_task(task& inner) noexcept : inner_(&inner) {}
    shared_task(const shared_task&) = delete;
    shared_task& operator=(const shared_task&) = delete;
    /// Destroy only while not running and without waiters (Debug asserts).
    ~shared_task() {
        STEPWRIGHT_ASSERT(state_.load(std::memory_order_acquire) != state_running &&
                              waiters_.empty(),
                          "destroying a running shared_task or one with waiters");
    }

    /// Return a waiter task bound to this shared task (guaranteed copy elision).
    waiter wait() noexcept;
    /// Thread-safe; true once the inner task has completed (until reset()).
    bool is_done() const noexcept { return state_.load(std::memory_order_acquire) == state_done; }
    /// Allow another run. Precondition: is_done(), no waiters, not concurrent with
    /// any other member call.
    void reset() noexcept {
        const std::lock_guard lock(lock_);
        STEPWRIGHT_ASSERT(state_.load(std::memory_order_relaxed) == state_done && waiters_.empty(),
                          "shared_task reset while not done or with waiters");
        state_.store(state_idle, std::memory_order_relaxed);
        result_ = {};
    }
    /// Request cancellation of the inner task if it is running; otherwise no-op.
    /// Thread-safe with waits and inner completion; do not race it with reset().
    void cancel() noexcept {
        std::optional<cancel_source> source;
        {
            const std::lock_guard lock(lock_);
            if (state_.load(std::memory_order_relaxed) == state_running) {
                source = source_;
            }
        }
        if (source) {
            source->request_cancel();
        }
    }

private:
    static constexpr std::uint8_t state_idle = 0;
    static constexpr std::uint8_t state_running = 1;
    static constexpr std::uint8_t state_done = 2;

    void on_inner_done(const error& result) noexcept;

    task* inner_;
    detail::spin_lock lock_;
    std::atomic<std::uint8_t> state_{state_idle};
    error result_;
    detail::wait_list waiters_;
    std::optional<cancel_source> source_;
};

/// Task completing with the shared inner result (starting the inner task if it
/// is the first waiter of a run), or errc::cancelled when its own token is
/// cancelled first. Restartable after completion; must outlive its completion
/// (Debug asserts on destruction while started).
class shared_task::waiter final : public detail::async_op, private detail::wait_link {
public:
    /// Bind to owner without registering.
    explicit waiter(shared_task& owner) noexcept : owner_(&owner) {}
    waiter(const waiter&) = delete;
    waiter& operator=(const waiter&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~waiter() override = default;

private:
    friend class shared_task;
    static waiter& from(detail::wait_link* link) noexcept { return static_cast<waiter&>(*link); }

    void on_start(completion done, cancel_token token) override {
        shared_task& owner = *owner_;
        bool start_inner = false;
        {
            std::unique_lock lock(owner.lock_);
            const auto state = owner.state_.load(std::memory_order_relaxed);
            if (state == shared_task::state_done) {
                const error result = owner.result_;
                lock.unlock();
                done(result);
                return;
            }
            if (state == shared_task::state_idle) {
                if (!owner.source_) {
                    owner.source_.emplace();
                } else if (owner.source_->cancel_requested()) {
                    detail::cancel_source_access::reset(*owner.source_);
                }
                owner.state_.store(shared_task::state_running, std::memory_order_relaxed);
                start_inner = true;
            }
            prepare(done, token);
            owner.waiters_.push_back(*this);
        }
        if (start_inner) {
            // Only the starter of a run touches source_ while running.
            owner.inner_->start(completion::bind<&shared_task::on_inner_done>(&owner),
                                owner.source_->token());
        }
        if (token.can_be_cancelled()) {
            arm(token, &on_cancel);
        }
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<waiter*>(context);
        shared_task& owner = *self.owner_;
        bool removed = false;
        {
            const std::lock_guard lock(owner.lock_);
            if (detail::wait_list::linked(self)) {
                owner.waiters_.remove(self);
                removed = true;
            }
        }
        if (removed) {
            self.resolve(error::make(errc::cancelled));
        }
    }

    shared_task* owner_;
};

inline shared_task::waiter shared_task::wait() noexcept {
    return waiter(*this);
}

inline void shared_task::on_inner_done(const error& result) noexcept {
    detail::wait_link* chain = nullptr;
    {
        const std::lock_guard lock(lock_);
        result_ = result;
        state_.store(state_done, std::memory_order_release);
        chain = waiters_.take_all();
    }
    // No access to *this after the waiters are detached: a receiver may destroy it.
    const detail::handoff_scope batch;
    while (chain) {
        detail::wait_link* next = chain->next;
        waiter::from(chain).resolve(result);
        chain = next;
    }
}
} // namespace stepwright
