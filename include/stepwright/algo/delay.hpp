#pragma once

#include <stepwright/detail/async_op.hpp>
#include <stepwright/timer.hpp>

namespace stepwright {
/// Task completing successfully once duration d has elapsed on a timer service,
/// or with errc::cancelled (the timer is cancelled) when its token is cancelled
/// first; exactly one outcome. Completion is delivered through the hand-off
/// trampoline on the firing thread (the timer thread for steady_timer_service).
/// Restartable after completion; must outlive its completion (Debug asserts) and
/// the service must outlive the delay. No allocation on success.
class delay final : public detail::async_op, private timer_service::entry {
public:
    /// Bind to timers and a relative duration; nothing is scheduled until start.
    delay(timer_service& timers, timer_service::clock::duration d) noexcept
        : timers_(&timers)
        , duration_(d) {
        fire = &on_fire;
    }
    delay(const delay&) = delete;
    delay& operator=(const delay&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~delay() override = default;

private:
    void on_start(completion done, cancel_token token) override {
        prepare(done, token);
        timers_->schedule(*this, timers_->now() + duration_);
        if (token.can_be_cancelled()) {
            arm(token, &on_cancel);
        }
    }
    static void on_fire(timer_service::entry* e) noexcept {
        auto& self = static_cast<delay&>(*e);
        if (self.decide()) {
            self.resolve({});
        }
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<delay*>(context);
        if (self.decide()) {
            // Waits for a callback running on another thread; that callback has
            // lost the decision and only returns.
            self.timers_->cancel(self);
            self.resolve(error::make(errc::cancelled));
        }
    }

    timer_service* timers_;
    timer_service::clock::duration duration_;
};
} // namespace stepwright
