#pragma once

#include <stepwright/detail/handoff.hpp>
#include <stepwright/task.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <utility>

namespace stepwright::detail {
// Shared completion protocol for waiting operations (event/shared_task waiters,
// mutex lock operations, delay). The waiter is published (linked into a primitive
// or scheduled on a timer) *before* its cancellation registration is armed, so a
// resolution can race with arming. One atomic phase word decides who completes:
//
//   resolver:  result_ = r; old = fetch_or(resolved); if (old & armed) finish();
//   starter:   arm(token);  old = fetch_or(armed);    if (old & resolved) finish();
//
// Exactly one side sees the other's bit, so exactly one calls finish(). With an
// empty token `armed` is set before publication and the resolver always finishes.
// Exactly one resolver exists: the party that unlinked the node under the
// primitive's lock (grant or cancellation), or the winner of `decided` (timers).
// finish() disarms cancellation (waiting for a callback running elsewhere), then
// delivers the receiver through the hand-off trampoline.
class async_op : public task, private work_item {
public:
    async_op(const async_op&) = delete;
    async_op& operator=(const async_op&) = delete;
    /// Destroy only while idle (not started, or completed); Debug asserts.
    ~async_op() override {
        STEPWRIGHT_ASSERT(phase_.load(std::memory_order_acquire) == 0,
                          "destroying a started stepwright waiter");
    }

protected:
    static constexpr std::uint8_t started = 1;
    static constexpr std::uint8_t armed = 2;
    static constexpr std::uint8_t resolved = 4;
    static constexpr std::uint8_t decided = 8;

    async_op() noexcept = default;

    /// Record the receiver before the operation is published.
    void prepare(completion done, const cancel_token& token) noexcept {
        receiver_ = done;
        phase_.store(token.can_be_cancelled() ? started : started | armed,
                     std::memory_order_relaxed);
    }
    /// Arm cancellation after publication; the last access by the starter. With an
    /// empty token this does not touch the object (it may already be complete).
    void arm(const cancel_token& token, void (*on_cancel)(void*) noexcept) noexcept {
        if (!token.can_be_cancelled()) {
            return;
        }
        cancellation_.arm(token, on_cancel, this);
        if ((phase_.fetch_or(armed, std::memory_order_acq_rel) & resolved) != 0) {
            finish();
        }
    }
    /// Claim the single resolution right among racing resolvers (timer vs cancel).
    bool decide() noexcept {
        return (phase_.fetch_or(decided, std::memory_order_acq_rel) & decided) == 0;
    }
    /// Store the result; complete if the starter has finished arming.
    void resolve(error result) noexcept {
        result_ = std::move(result);
        if ((phase_.fetch_or(resolved, std::memory_order_acq_rel) & armed) != 0) {
            finish();
        }
    }

private:
    void finish() noexcept {
        cancellation_.disarm();
        work_item::run = &deliver;
        handoff(static_cast<work_item&>(*this));
    }
    static void deliver(work_item* item) noexcept {
        if (!item) {
            std::terminate();
        }
        auto& self = static_cast<async_op&>(*item);
        const error result = std::move(self.result_);
        const completion receiver = self.receiver_;
        self.phase_.store(0, std::memory_order_relaxed);
        receiver(result);
    }

    completion receiver_;
    cancel_registration cancellation_;
    error result_;
    std::atomic<std::uint8_t> phase_{0};
};
} // namespace stepwright::detail
