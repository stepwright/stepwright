#pragma once

#include <stepwright/detail/wait_core.hpp>
#include <stepwright/stream/source.hpp>
#include <stepwright/timer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Source that ends immediately. Always ready.
template <class T>
class empty_source final : public source<T> {
public:
    empty_source() noexcept = default;
    /// Always end.
    pull<T> try_pull() override { return pull<T>::end(); }
    /// Completes synchronously.
    void on_ready(completion done, cancel_token /*token*/) override { done({}); }
};

/// Source emitting N values stored inline, then ending. Always ready; one pass.
template <class T, std::size_t N>
class just_source final : public source<T> {
public:
    /// Store the values.
    explicit just_source(std::array<T, N> values) noexcept(std::is_nothrow_move_constructible_v<T>)
        : values_(std::move(values)) {}
    /// The next value, then end (sticky).
    pull<T> try_pull() override {
        if (index_ == N) {
            return pull<T>::end();
        }
        return pull<T>::of(std::move(values_[index_++]));
    }
    /// Completes synchronously.
    void on_ready(completion done, cancel_token /*token*/) override { done({}); }

private:
    std::array<T, N> values_;
    std::size_t index_ = 0;
};

/// Source emitting copies of the elements of a range (non-owning: the range must
/// outlive the source and stay unchanged), then ending. Always ready; one pass.
template <std::ranges::input_range R>
class range_source final : public source<std::ranges::range_value_t<R>> {
public:
    using value_type = std::ranges::range_value_t<R>; ///< Item type.
    /// Iterate r (non-owning).
    explicit range_source(R& r) : it_(std::ranges::begin(r)), end_(std::ranges::end(r)) {}
    /// The next element, then end (sticky).
    pull<value_type> try_pull() override {
        if (it_ == end_) {
            return pull<value_type>::end();
        }
        pull<value_type> result = pull<value_type>::of(value_type(*it_));
        ++it_;
        return result;
    }
    /// Completes synchronously.
    void on_ready(completion done, cancel_token /*token*/) override { done({}); }

private:
    std::ranges::iterator_t<R> it_;
    std::ranges::sentinel_t<R> end_;
};

/// Source calling f() for each pull: f returns std::optional<T>; nullopt ends the
/// stream (f is not called again). Always ready (a pull always makes progress).
template <class F>
class generate_source final : public source<typename std::invoke_result_t<F&>::value_type> {
public:
    using value_type = typename std::invoke_result_t<F&>::value_type; ///< Item type.
    /// Store f by value.
    explicit generate_source(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(f)) {}
    /// Call f, or end (sticky) after it returned nullopt.
    pull<value_type> try_pull() override {
        if (ended_) {
            return pull<value_type>::end();
        }
        std::optional<value_type> next = std::invoke(fn_);
        if (!next) {
            ended_ = true;
            return pull<value_type>::end();
        }
        return pull<value_type>::of(std::move(*next));
    }
    /// Completes synchronously.
    void on_ready(completion done, cancel_token /*token*/) override { done({}); }

private:
    F fn_;
    bool ended_ = false;
};

/// Periodic tick source on a timer service: tick k (k = 0, 1, 2, ...) is due at
/// creation time + (k + 1) * period. try_pull returns the latest due tick number,
/// or pending before it. Missed ticks coalesce: a consumer that falls behind gets
/// one item carrying the number of the most recent due tick, so tick numbers skip
/// by the number of missed periods. Readiness schedules the embedded timer entry
/// for the next due tick (no allocation); cancellation removes it. Never ends.
/// The timer service must outlive the source; destroy only without an outstanding
/// readiness wait (Debug asserts).
class interval_source final : public source<std::uint64_t>, private timer_service::entry {
public:
    /// Start the period now; period must be positive (Debug asserts).
    interval_source(timer_service& timers, timer_service::clock::duration period) noexcept
        : timers_(&timers)
        , period_(period)
        , start_(timers.now()) {
        STEPWRIGHT_ASSERT(period.count() > 0, "interval period must be positive");
        fire = &on_fire;
    }
    /// Destroy only without an outstanding readiness wait.
    ~interval_source() override {
        STEPWRIGHT_ASSERT(wait_.idle(), "destroying a source with an outstanding readiness wait");
    }
    /// The latest due tick number, or pending.
    pull<std::uint64_t> try_pull() override {
        STEPWRIGHT_ASSERT(wait_.idle(), "try_pull with an outstanding readiness wait");
        const auto now = timers_->now();
        if (now < due(next_)) {
            return pull<std::uint64_t>::pending();
        }
        const auto elapsed = static_cast<std::uint64_t>((now - start_) / period_);
        next_ = elapsed;
        return pull<std::uint64_t>::of(elapsed - 1);
    }
    /// Complete when the next tick is due (now, or from the timer service's firing
    /// thread), or with errc::cancelled.
    void on_ready(completion done, cancel_token token) override {
        STEPWRIGHT_ASSERT(wait_.idle(), "concurrent readiness waits on a source");
        const auto next_due = due(next_);
        if (timers_->now() >= next_due) {
            done({});
            return;
        }
        wait_.prepare(done, token);
        timers_->schedule(*this, next_due);
        wait_.arm(token, &on_cancel, this);
    }
    /// The period.
    timer_service::clock::duration period() const noexcept { return period_; }

private:
    timer_service::clock::time_point due(std::uint64_t tick) const noexcept {
        return start_ + period_ * static_cast<timer_service::clock::rep>(tick + 1);
    }
    static void on_fire(timer_service::entry* e) noexcept {
        auto& self = static_cast<interval_source&>(*e);
        if (self.wait_.decide()) {
            self.wait_.resolve({});
        }
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<interval_source*>(context);
        if (self.wait_.decide()) {
            // Waits for a fire callback running elsewhere; it has lost and returns.
            self.timers_->cancel(self);
            self.wait_.resolve(detail::cancelled_error());
        }
    }

    timer_service* timers_;
    timer_service::clock::duration period_;
    timer_service::clock::time_point start_;
    std::uint64_t next_ = 0; // next tick number to emit at the earliest
    detail::wait_core wait_;
};

/// A source<T> that ends immediately.
template <class T>
empty_source<T> empty() noexcept {
    return {};
}
/// A source emitting the given values (stored inline), then ending.
template <class V0, class... V>
just_source<std::common_type_t<std::decay_t<V0>, std::decay_t<V>...>, 1 + sizeof...(V)>
just(V0&& first, V&&... rest) {
    using value_type = std::common_type_t<std::decay_t<V0>, std::decay_t<V>...>;
    return just_source<value_type, 1 + sizeof...(V)>(std::array<value_type, 1 + sizeof...(V)>{
        value_type(std::forward<V0>(first)), value_type(std::forward<V>(rest))...});
}
/// A source emitting copies of r's elements (non-owning; r must outlive it).
template <std::ranges::input_range R>
range_source<R> from_range(R& r) {
    return range_source<R>(r);
}
/// Rejected: a temporary range would dangle.
template <class R>
void from_range(const R&& r) = delete;
/// A source calling f() (returning std::optional<T>) for each pull.
template <class F>
generate_source<std::decay_t<F>> generate(F&& f) {
    return generate_source<std::decay_t<F>>(std::forward<F>(f));
}
/// A tick source emitting the latest due tick number every period.
inline interval_source interval(timer_service& timers,
                                timer_service::clock::duration period) noexcept {
    return {timers, period};
}
} // namespace stepwright
