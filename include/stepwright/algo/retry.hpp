#pragma once

#include <stepwright/algo/delay.hpp>
#include <stepwright/machine.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace stepwright {
/// Deterministic exponential backoff; durations must be nonnegative and
/// multiplier finite and >= 1. No jitter. max_attempts counts the first run too.
struct retry_policy {
    std::size_t max_attempts = 3; ///< At least one total attempt.
    /// Delay after the first retryable failure, capped by max_backoff.
    timer_service::clock::duration initial_backoff = std::chrono::milliseconds(100);
    double multiplier = 2.0; ///< Finite factor, at least 1.0.
    /// Maximum delay between attempts, including the first backoff.
    timer_service::clock::duration max_backoff = std::chrono::seconds(30);
    /// Non-null predicate, called only on failure with attempts remaining.
    /// Default: retry every failure except cooperative cancellation.
    bool (*should_retry)(const error&) noexcept = [](const error& e) noexcept {
        return !e.is(errc::cancelled);
    };
};

/// Run a borrowed task until success, a nonretryable failure or exhaustion.
/// Backoff uses an embedded cancellable delay; cancellation during it prevents
/// another attempt. The start token reaches inner and delay. Restartable, constant
/// stack, no allocation by the wrapper on runs (errors/context may allocate).
/// inner and timers must outlive every run; inner must itself be restartable.
class retry final : public machine<retry> {
public:
    /// Bind inner/timers and copy a policy; no allocation or work is started.
    retry(task& inner, timer_service& timers, retry_policy policy = {}) noexcept
        : inner_(&inner)
        , pause_(timers, {})
        , policy_(policy) {
        STEPWRIGHT_ASSERT(policy.max_attempts >= 1, "retry requires at least one attempt");
        STEPWRIGHT_ASSERT(std::isfinite(policy.multiplier) && policy.multiplier >= 1.0,
                          "retry multiplier must be finite and at least one");
        STEPWRIGHT_ASSERT(policy.initial_backoff >= duration::zero() &&
                              policy.max_backoff >= duration::zero(),
                          "retry backoff must be nonnegative");
        STEPWRIGHT_ASSERT(policy.should_retry, "retry requires a predicate");
    }
    /// Attempts made in the current/last run; read after synchronising completion
    /// or on the driving thread, not concurrently with a running attempt.
    std::size_t attempts() const noexcept { return attempts_; }

private:
    friend class machine<retry>;
    using duration = timer_service::clock::duration;
    step begin() noexcept {
        attempts_ = 0;
        backoff_ = std::min(policy_.initial_backoff, policy_.max_backoff);
        return then<&retry::attempt>();
    }
    step attempt() noexcept {
        if (cancel_requested()) {
            return fail(errc::cancelled);
        }
        ++attempts_;
        return after<&retry::finished>(*inner_);
    }
    step finished(const error& result) noexcept {
        // The error argument is deliberate: this step decides whether to retry.
        if (!result) {
            return done();
        }
        if (attempts_ >= policy_.max_attempts || !policy_.should_retry(result)) {
            return fail(result);
        }
        pause_.with(backoff_);
        // Compare before converting to the integral clock representation, avoiding
        // overflow even with a huge multiplier or duration::max() cap.
        const long double next_count = static_cast<long double>(backoff_.count()) *
                                       static_cast<long double>(policy_.multiplier);
        backoff_ = next_count >= static_cast<long double>(policy_.max_backoff.count())
                       ? policy_.max_backoff
                       : duration(static_cast<duration::rep>(next_count));
        return after<&retry::attempt>(pause_);
    }
    task* inner_;
    delay pause_;
    retry_policy policy_;
    duration backoff_{};
    std::size_t attempts_ = 0;
};
} // namespace stepwright
