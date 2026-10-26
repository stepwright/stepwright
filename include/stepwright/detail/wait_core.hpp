#pragma once

#include <stepwright/cancel.hpp>
#include <stepwright/completion.hpp>
#include <stepwright/detail/handoff.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>

namespace stepwright::detail {
// Completion protocol shared by every waiting operation: primitive waiters (through
// async_op) and the readiness waits of stream sources. The waiter is published (linked
// into a primitive, scheduled on a timer, or flagged in a source's state word)
// *before* its cancellation registration is armed, so a resolution can race with
// arming. One atomic phase word decides who completes:
//
//   resolver:  result_ = r; old = fetch_or(resolved); if (old & armed) finish();
//   starter:   arm(token);  old = fetch_or(armed);    if (old & resolved) finish();
//
// Exactly one side sees the other's bit, so exactly one calls finish(). With an
// empty token `armed` is set before publication and the resolver always finishes;
// the starter must skip arm (and every other member access) after publication.
// Exactly one resolver exists per wait: the party that unpublished the waiter
// under the owner's lock or CAS (grant or cancellation), or the winner of
// `decided` (timers). finish() disarms cancellation (waiting for a callback
// running elsewhere), then delivers the receiver through the hand-off trampoline.
// The phase returns to 0 before the receiver runs, so the receiver may restart or
// destroy the waiter. Layout is independent of NDEBUG.
class wait_core : private work_item {
public:
    wait_core() noexcept = default;
    wait_core(const wait_core&) = delete;
    wait_core& operator=(const wait_core&) = delete;
    /// Destroy only while idle (not started, or completed); Debug asserts.
    ~wait_core() {
        STEPWRIGHT_ASSERT(phase_.load(std::memory_order_acquire) == 0,
                          "destroying a started stepwright waiter");
    }

    static constexpr std::uint8_t started = 1;
    static constexpr std::uint8_t armed = 2;
    static constexpr std::uint8_t resolved = 4;
    static constexpr std::uint8_t decided = 8;

    /// Whether no wait is in progress (owner-thread query, acquire).
    bool idle() const noexcept { return phase_.load(std::memory_order_acquire) == 0; }
    /// Record the receiver before the wait is published.
    void prepare(completion done, const cancel_token& token) noexcept {
        receiver_ = done;
        phase_.store(token.can_be_cancelled() ? started : started | armed,
                     std::memory_order_relaxed);
    }
    /// Abandon a prepared wait that was never published (nobody can resolve it).
    void unprepare() noexcept { phase_.store(0, std::memory_order_relaxed); }
    /// Arm cancellation after publication; the last access by the starter. The
    /// caller must check its own token and skip this call if it cannot cancel:
    /// publication may already have completed and ended this object's lifetime.
    void arm(const cancel_token& token, void (*on_cancel)(void*) noexcept, void* ctx) noexcept {
        if (!token.can_be_cancelled()) {
            return;
        }
        // Idle: the previous wait's finish() disarmed it.
        cancellation_.bind(token, on_cancel, ctx);
        cancellation_.activate();
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
        auto& self = static_cast<wait_core&>(*item);
        const error result = std::move(self.result_);
        const completion receiver = self.receiver_;
        self.phase_.store(0, std::memory_order_release);
        receiver(result);
    }

    completion receiver_;
    cancel_registration cancellation_;
    error result_;
    std::atomic<std::uint8_t> phase_{0};
};

/// Shared errc::cancelled value for cancellation paths that must not allocate in
/// steady state (stream readiness waits). Copies share one payload; callers that
/// add context clone it first (error's copy-on-write), so it is never mutated.
/// Immortal: the handle is constructed once in static storage and never destroyed,
/// so threads still running during static destruction (detached threads, timer
/// services owned by other statics) can keep copying it. Its payload stays
/// reachable through that storage, so leak checkers do not report it.
inline const error& cancelled_error() noexcept {
    alignas(error) static std::array<std::byte, sizeof(error)> storage;
    // A trivially destructible pointer: no exit-time destructor is registered.
    static const error* const value =
        ::new (static_cast<void*>(storage.data())) error(error::make(errc::cancelled));
    return *value;
}
} // namespace stepwright::detail
