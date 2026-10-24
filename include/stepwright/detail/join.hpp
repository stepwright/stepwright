#pragma once

#include <stepwright/task.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace stepwright {
/// What when_all does with the remaining children after the first failure.
enum class when_all_policy : std::uint8_t {
    cancel_on_error, ///< Request cancellation of the remaining children.
    run_all          ///< Let every child run to completion undisturbed.
};

namespace detail {
// Shared engine of when_all / when_any.
//
// count_ packs two fields. Bits 0..39 count outstanding *units*: one per child,
// one held by the starter until every child has been started, one held by the
// when_any winner while it requests cancellation, and one held temporarily by
// the parent-cancellation callback. Whoever releases the last unit completes the
// combinator; holding a unit therefore keeps it alive across request_cancel()
// and guarantees that no request on the internal source is still running when
// the next start resets it. Bits 40.. count child completions (when_any only),
// so one RMW both releases a child's unit and tells it whether it came first.
class join_base : public task {
public:
    /// winner() before any completion (or for an empty when_any without checks).
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    join_base(const join_base&) = delete;
    join_base& operator=(const join_base&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~join_base() override {
        STEPWRIGHT_ASSERT((count_.load(std::memory_order_acquire) & unit_mask) == 0,
                          "destroying a running when_all/when_any");
    }

protected:
    enum class mode : std::uint8_t {
        all_cancel_on_error,
        all_run_all,
        any
    };
    struct slot {
        join_base* owner = nullptr;
    };

    explicit join_base(mode m) : mode_(m) {}
    static mode all_mode(when_all_policy policy) noexcept {
        return policy == when_all_policy::run_all ? mode::all_run_all : mode::all_cancel_on_error;
    }
    void attach(std::span<task* const> children, slot* slots) noexcept {
        children_ = children;
        slots_ = slots;
        if (slots_) {
            for (std::size_t i = 0; i < children_.size(); ++i) {
                slots_[i].owner = this;
            }
        }
    }
    std::size_t winner_index() const noexcept { return winner_; }

private:
    static constexpr std::uint64_t unit_mask = (std::uint64_t{1} << 40) - 1;
    static constexpr std::uint64_t completion_one = std::uint64_t{1} << 40;

    void on_start(completion done, cancel_token token) final {
        const std::size_t n = children_.size();
        if (n == 0) {
            STEPWRIGHT_ASSERT(mode_ != mode::any, "when_any requires at least one child");
            done({});
            return;
        }
        receiver_ = done;
        if (source_.cancel_requested()) {
            source_.reset();
        }
        result_ = {};
        winner_ = npos;
        failed_.store(false, std::memory_order_relaxed);
        parent_cancelled_.store(false, std::memory_order_relaxed);
        count_.store(n + (mode_ == mode::any ? 2 : 1), std::memory_order_relaxed);
        linked_ = token.can_be_cancelled();
        if (linked_) {
            link_.arm(token, &on_parent_cancel, this);
        }
        const cancel_token child_token =
            mode_ == mode::all_run_all && !linked_ ? cancel_token{} : source_.token();
        for (std::size_t i = 0; i < n; ++i) {
            const completion target = mode_ == mode::any
                                          ? completion{.fn = &on_child_any, .ctx = &slots_[i]}
                                          : completion{.fn = &on_child_all, .ctx = this};
            children_[i]->start(target, child_token);
        }
        release();
    }
    void release() noexcept {
        if ((count_.fetch_sub(1, std::memory_order_acq_rel) & unit_mask) == 1) {
            finish();
        }
    }
    static void on_child_all(void* context, const error& result) noexcept {
        auto& self = *static_cast<join_base*>(context);
        if (result) {
            // The failing child's unit is held until the request has returned.
            if (!self.failed_.exchange(true, std::memory_order_acq_rel)) {
                self.result_ = result;
                if (self.mode_ == mode::all_cancel_on_error) {
                    self.source_.request_cancel();
                }
            }
        }
        self.release();
    }
    static void on_child_any(void* context, const error& result) noexcept {
        auto& entry = *static_cast<slot*>(context);
        auto& self = *entry.owner;
        const auto old = self.count_.fetch_add(completion_one - 1, std::memory_order_acq_rel);
        if ((old >> 40) == 0) {
            // First completion: the winner unit keeps the combinator alive.
            self.winner_ = static_cast<std::size_t>(&entry - self.slots_);
            self.result_ = result;
            if (self.children_.size() > 1) {
                self.source_.request_cancel();
            }
            self.release();
        } else if ((old & unit_mask) == 1) {
            self.finish();
        }
    }
    static void on_parent_cancel(void* context) noexcept {
        auto& self = *static_cast<join_base*>(context);
        const auto old = self.count_.fetch_add(1, std::memory_order_acq_rel);
        if ((old & unit_mask) == 0) {
            // Completion already decided; its finisher waits in disarm() for us.
            self.count_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        self.parent_cancelled_.store(true, std::memory_order_relaxed);
        self.source_.request_cancel();
        self.release();
    }
    void finish() noexcept {
        if (linked_) {
            link_.disarm();
        }
        error result;
        if (mode_ == mode::any || failed_.load(std::memory_order_relaxed)) {
            result = std::move(result_);
        } else if (parent_cancelled_.load(std::memory_order_relaxed)) {
            result = error::make(errc::cancelled);
        }
        const completion receiver = receiver_;
        receiver(result);
    }

    std::span<task* const> children_;
    slot* slots_ = nullptr;
    completion receiver_;
    cancel_source source_;
    cancel_registration link_;
    error result_;
    std::size_t winner_ = npos;
    std::atomic<std::uint64_t> count_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> parent_cancelled_{false};
    bool linked_ = false;
    mode mode_;
};
} // namespace detail
} // namespace stepwright
