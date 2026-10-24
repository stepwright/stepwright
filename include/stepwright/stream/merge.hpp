#pragma once

#include <stepwright/detail/wait_core.hpp>
#include <stepwright/stream/source.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace stepwright {
namespace detail {
// Shared engine of merge<T, N> and merge_range<T>.
//
// try_pull (consumer only) polls the live inputs from a rotating index and owns
// live_/next_/failure state. A readiness wait (a "round") is only needed when
// every live input returned pending: it arms on_ready on every live input with a
// token from an internal cancel_source and counts *units*: one per armed input
// plus one held by the starter while arming, plus one held temporarily by the
// consumer's cancellation callback. The first input to become ready (or the
// consumer's cancellation) decides the round with one CAS and requests the
// internal source, cancelling the other input waits; units are held across that
// request, so the round completes (and the source can be reset for the next
// round) only after every input wait has completed and every request returned.
template <class T>
class merge_core : public source<T> {
public:
    merge_core(const merge_core&) = delete;
    merge_core& operator=(const merge_core&) = delete;
    /// Destroy only without an outstanding readiness wait (Debug asserts).
    ~merge_core() override {
        STEPWRIGHT_ASSERT(consumer_.idle(),
                          "destroying a source with an outstanding readiness wait");
    }

    /// Poll the live inputs starting after the input that produced the previous
    /// item; return the first item, end when every input has ended, the first
    /// failure (sticky; no input is polled afterwards), otherwise pending.
    pull<T> try_pull() final {
        STEPWRIGHT_ASSERT(consumer_.idle(), "try_pull with an outstanding readiness wait");
        if (failed_) {
            return pull<T>::failure(failure_);
        }
        const std::size_t size = slots_.size();
        std::size_t index = next_;
        for (std::size_t k = 0; k < size && live_ != 0;
             ++k, index = index + 1 == size ? 0 : index + 1) {
            slot& entry = slots_[index];
            if (!entry.live) {
                continue;
            }
            pull<T> result = entry.input->try_pull();
            switch (result.status()) {
                case pull<T>::state::item:
                    next_ = index + 1 == size ? 0 : index + 1;
                    return result;
                case pull<T>::state::pending:
                    break;
                case pull<T>::state::end:
                    entry.live = false;
                    --live_;
                    break;
                case pull<T>::state::failed:
                    failed_ = true;
                    failure_ = result.failure_reason();
                    return result;
            }
        }
        return live_ == 0 ? pull<T>::end() : pull<T>::pending();
    }
    /// Complete when any live input is ready (or immediately after end/failure):
    /// arms readiness on every live input, completes after the first is ready and
    /// the others have been cancelled and have completed. errc::cancelled when the
    /// token is cancelled first.
    void on_ready(completion done, cancel_token token) final {
        STEPWRIGHT_ASSERT(consumer_.idle(), "concurrent readiness waits on a source");
        if (failed_ || live_ == 0) {
            done({});
            return;
        }
        if (source_.cancel_requested()) {
            cancel_source_access::reset(source_);
        }
        const std::size_t live = live_;
        decision_.store(undecided, std::memory_order_relaxed);
        units_.store(live + 1, std::memory_order_relaxed);
        consumer_.prepare(done, token);
        const cancel_token inputs_token = source_.token();
        const std::size_t size = slots_.size();
        std::size_t armed = 0;
        std::size_t index = next_;
        for (std::size_t k = 0; k < size && armed < live;
             ++k, index = index + 1 == size ? 0 : index + 1) {
            slot& entry = slots_[index];
            if (!entry.live) {
                continue;
            }
            if (decision_.load(std::memory_order_acquire) != undecided) {
                break; // an input was ready synchronously: stop arming
            }
            ++armed;
            entry.input->on_ready({.fn = &on_input, .ctx = &entry}, inputs_token);
        }
        // Release the starter's unit and those of inputs that were not armed. The
        // round may complete here; with an empty token it may also deliver.
        const bool cancellable = token.can_be_cancelled();
        release(live - armed + 1);
        if (cancellable) {
            consumer_.arm(token, &on_consumer_cancel, this);
        }
    }

protected:
    struct slot {
        merge_core* owner = nullptr;
        source<T>* input = nullptr;
        bool live = true;
    };

    /// Allocates the internal cancel source once.
    merge_core() = default;
    void attach(std::span<slot> slots) noexcept {
        slots_ = slots;
        live_ = slots.size();
        for (auto& entry : slots_) {
            entry.owner = this;
            entry.live = entry.input != nullptr;
            if (!entry.live) {
                --live_;
            }
        }
    }

private:
    static constexpr std::uint8_t undecided = 0;
    static constexpr std::uint8_t ready = 1;
    static constexpr std::uint8_t cancelled = 2;

    bool decide(std::uint8_t outcome) noexcept {
        std::uint8_t expected = undecided;
        return decision_.compare_exchange_strong(expected, outcome, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
    }
    void release(std::size_t units) noexcept {
        if (units_.fetch_sub(units, std::memory_order_acq_rel) == units) {
            const bool was_cancelled = decision_.load(std::memory_order_acquire) == cancelled;
            consumer_.resolve(was_cancelled ? cancelled_error() : error{});
        }
    }
    static void on_input(void* context, const error& result) noexcept {
        auto& entry = *static_cast<slot*>(context);
        merge_core& self = *entry.owner;
        // Holding this input's unit keeps the round open across the request.
        if (!result.is(errc::cancelled) && self.decide(ready)) {
            self.source_.request_cancel();
        }
        self.release(1);
    }
    static void on_consumer_cancel(void* context) noexcept {
        auto& self = *static_cast<merge_core*>(context);
        if (self.units_.fetch_add(1, std::memory_order_acq_rel) == 0) {
            // The round is already complete; its finisher waits in disarm() for us.
            self.units_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        if (self.decide(cancelled)) {
            self.source_.request_cancel();
        }
        self.release(1);
    }

    std::span<slot> slots_;
    std::size_t live_ = 0;
    std::size_t next_ = 0;
    bool failed_ = false;
    error failure_;
    cancel_source source_;
    wait_core consumer_;
    std::atomic<std::size_t> units_{0};
    std::atomic<std::uint8_t> decision_{undecided};
};

template <class S>
concept any_source =
    requires { typename S::value_type; } && std::derived_from<S, source<typename S::value_type>>;
} // namespace detail

/// Fair fan-in of N sources of T without head-of-line blocking (see
/// detail::merge_core): try_pull polls from a rotating index; when every live
/// input is pending, the readiness wait arms all of them and completes on the
/// first. Ends when every input has ended; the first input failure fails the merge
/// (sticky). The merge is the single consumer of its inputs, which must outlive
/// it. Construction allocates the internal cancel source once; no allocation per
/// item or readiness round.
template <class T, std::size_t N>
class merge final : public detail::merge_core<T> {
public:
    /// Merge the given inputs (non-owning).
    explicit merge(std::array<source<T>*, N> inputs) {
        for (std::size_t i = 0; i < N; ++i) {
            slots_[i].input = inputs[i];
        }
        this->attach(slots_);
    }
    /// Merge inputs given by reference.
    template <class... S>
        requires(sizeof...(S) == N && (std::derived_from<S, source<T>> && ...))
    explicit merge(S&... inputs) : merge(std::array<source<T>*, N>{&inputs...}) {}

private:
    std::array<typename detail::merge_core<T>::slot, N> slots_{};
};
template <class S0, class... S>
    requires(detail::any_source<S0> &&
             (std::derived_from<S, source<typename S0::value_type>> && ...))
merge(S0&, S&...) -> merge<typename S0::value_type, 1 + sizeof...(S)>;

/// merge over a runtime-sized span of inputs. Per-input state is allocated once at
/// construction; the inputs must outlive the merge. Same semantics as merge<T, N>.
template <class T>
class merge_range final : public detail::merge_core<T> {
public:
    /// Merge the given inputs (non-owning).
    explicit merge_range(std::span<source<T>* const> inputs) : slots_(inputs.size()) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            slots_[i].input = inputs[i];
        }
        this->attach(slots_);
    }

private:
    std::vector<typename detail::merge_core<T>::slot> slots_;
};
} // namespace stepwright
