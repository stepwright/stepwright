#pragma once

#include <stepwright/task.hpp>
#include <stepwright/timer.hpp>

#include <atomic>
#include <cstdint>
#include <optional>

namespace stepwright {
/// Run inner with a timeout. Starts inner with an internal token linked to the
/// start token, and schedules a timer. Exactly one outcome decides the result:
/// - inner first: cancel the timer (waiting for a callback in progress) and
///   complete with inner's result;
/// - timer first: request cancellation of inner, wait for inner to complete, and
///   complete with errc::timed_out;
/// - start-token cancellation first: cancel inner and the timer, wait for both,
///   and complete with errc::cancelled.
/// Never completes while inner or the timer callback is outstanding. Restartable
/// after completion; inner, the service and this object must outlive the run.
/// The first start allocates the internal cancel source; later runs do not
/// allocate when inner wins.
class with_deadline final : public task, private timer_service::entry {
public:
    /// Bind inner, the timer service and the timeout (all non-owning).
    with_deadline(task& inner, timer_service& timers,
                  timer_service::clock::duration timeout) noexcept
        : inner_(&inner)
        , timers_(&timers)
        , timeout_(timeout) {
        fire = &on_fire;
    }
    with_deadline(const with_deadline&) = delete;
    with_deadline& operator=(const with_deadline&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~with_deadline() override {
        STEPWRIGHT_ASSERT(units_.load(std::memory_order_acquire) == 0,
                          "destroying a running with_deadline");
    }
    /// Set the next run's timeout while idle (not concurrent with start).
    with_deadline& with(timer_service::clock::duration timeout) noexcept {
        STEPWRIGHT_ASSERT(units_.load(std::memory_order_acquire) == 0,
                          "with_deadline::with while active");
        timeout_ = timeout;
        return *this;
    }

private:
    static constexpr std::uint8_t undecided = 0;
    static constexpr std::uint8_t inner_won = 1;
    static constexpr std::uint8_t timer_won = 2;
    static constexpr std::uint8_t parent_won = 3;

    // Units: inner, timer and starter; the parent callback adds one temporarily.
    // Releasing the last unit completes the operation.
    void on_start(completion done, cancel_token token) override {
        receiver_ = done;
        if (!source_) {
            source_.emplace();
        } else if (source_->cancel_requested()) {
            detail::cancel_source_access::reset(*source_);
        }
        inner_result_ = {};
        outcome_.store(undecided, std::memory_order_relaxed);
        units_.store(3, std::memory_order_relaxed);
        linked_ = token.can_be_cancelled();
        timers_->schedule(*this, timers_->now() + timeout_);
        // The starter unit prevents completion until the final release below.
        if (token.can_be_cancelled()) {
            link_.arm(token, &on_parent_cancel, this);
        }
        inner_->start(completion::bind<&with_deadline::on_inner>(this), source_->token());
        release();
    }
    bool decide(std::uint8_t outcome) noexcept {
        std::uint8_t expected = undecided;
        return outcome_.compare_exchange_strong(expected, outcome, std::memory_order_acq_rel,
                                                std::memory_order_acquire);
    }
    void release() noexcept {
        if (units_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            finish();
        }
    }
    void on_inner(const error& result) noexcept {
        if (decide(inner_won)) {
            inner_result_ = result;
            // false: the callback ran or is running elsewhere (cancel waited for
            // it); having lost the decision it releases the timer unit itself.
            if (timers_->cancel(*this)) {
                release();
            }
        }
        release();
    }
    static void on_fire(timer_service::entry* e) noexcept {
        auto& self = static_cast<with_deadline&>(*e);
        if (self.decide(timer_won) && self.source_) {
            self.source_->request_cancel();
        }
        self.release();
    }
    static void on_parent_cancel(void* context) noexcept {
        auto& self = *static_cast<with_deadline*>(context);
        if (self.units_.fetch_add(1, std::memory_order_acq_rel) == 0) {
            // Completion already decided; its finisher waits in disarm() for us.
            self.units_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        if (self.decide(parent_won)) {
            if (self.source_) {
                self.source_->request_cancel();
            }
            if (self.timers_->cancel(self)) {
                self.release();
            }
        }
        self.release();
    }
    void finish() noexcept {
        if (linked_) {
            link_.disarm();
        }
        error result;
        switch (outcome_.load(std::memory_order_relaxed)) {
            case inner_won:
                result = std::move(inner_result_);
                break;
            case timer_won:
                result = error::make(errc::timed_out);
                break;
            default:
                result = error::make(errc::cancelled);
                break;
        }
        const completion receiver = receiver_;
        receiver(result);
    }

    task* inner_;
    timer_service* timers_;
    timer_service::clock::duration timeout_;
    completion receiver_;
    std::optional<cancel_source> source_;
    cancel_registration link_;
    error inner_result_;
    std::atomic<std::uint32_t> units_{0};
    std::atomic<std::uint8_t> outcome_{undecided};
    bool linked_ = false;
};
} // namespace stepwright
