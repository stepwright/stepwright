#pragma once

#include <stepwright/task.hpp>

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Result of a non-blocking pull from a source<T>: an item, `pending` (nothing
/// available yet: wait for readiness), `end` (the stream finished) or `failed`
/// (the stream finished with an error). Holds the item or the error inline; no
/// allocation. T must be nothrow move constructible.
// state_ selects value_ (item) or error_ (failed); pending/end hold neither.
template <class T>
// NOLINTNEXTLINE(bugprone-tagged-union-member-count): see above.
class pull {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                  "stream items must be nothrow move constructible and destructible");

public:
    /// Outcome of a pull.
    enum class state : std::uint8_t {
        item,    ///< value() holds the next item.
        pending, ///< Nothing available now; on_ready() signals when to pull again.
        end,     ///< The stream finished successfully (sticky).
        failed   ///< The stream finished with failure_reason() (sticky).
    };

    /// An item.
    static pull of(T value) noexcept { return pull(std::in_place, std::move(value)); }
    /// Nothing available yet.
    static pull pending() noexcept { return pull(state::pending); }
    /// The stream has ended.
    static pull end() noexcept { return pull(state::end); }
    /// The stream has failed with reason; an empty reason means end().
    static pull failure(error reason) noexcept {
        if (!reason) {
            return end();
        }
        pull result(state::failed);
        std::construct_at(&result.error_, std::move(reason));
        return result;
    }

    /// Move the item or error; other keeps its state with a moved-from payload.
    pull(pull&& other) noexcept : state_(other.state_) {
        if (state_ == state::item) {
            std::construct_at(&value_, std::move(other.value_));
        } else if (state_ == state::failed) {
            std::construct_at(&error_, std::move(other.error_));
        }
    }
    /// Copy (only for copyable T).
    pull(const pull& other) noexcept(std::is_nothrow_copy_constructible_v<T>)
        requires std::is_copy_constructible_v<T>
        : state_(other.state_) {
        if (state_ == state::item) {
            std::construct_at(&value_, other.value_);
        } else if (state_ == state::failed) {
            std::construct_at(&error_, other.error_);
        }
    }
    /// Replace the content by moving other's.
    pull& operator=(pull&& other) noexcept {
        if (this != &other) {
            destroy();
            state_ = other.state_;
            if (state_ == state::item) {
                std::construct_at(&value_, std::move(other.value_));
            } else if (state_ == state::failed) {
                std::construct_at(&error_, std::move(other.error_));
            }
        }
        return *this;
    }
    /// Copy assignment is not provided; move instead.
    pull& operator=(const pull&) = delete;
    /// Destroy the held item or error.
    ~pull() { destroy(); }

    /// The outcome.
    state status() const noexcept { return state_; }
    /// status() == state::item.
    bool has_item() const noexcept { return state_ == state::item; }
    /// status() == state::pending.
    bool is_pending() const noexcept { return state_ == state::pending; }
    /// status() == state::end.
    bool at_end() const noexcept { return state_ == state::end; }
    /// status() == state::failed.
    bool is_failed() const noexcept { return state_ == state::failed; }
    /// The item; precondition: has_item() (Debug asserts).
    T& value() & noexcept {
        STEPWRIGHT_ASSERT(has_item(), "pull::value without an item");
        return value_;
    }
    /// The item; precondition: has_item() (Debug asserts).
    const T& value() const& noexcept {
        STEPWRIGHT_ASSERT(has_item(), "pull::value without an item");
        return value_;
    }
    /// Move the item out; precondition: has_item() (Debug asserts).
    T&& value() && noexcept {
        STEPWRIGHT_ASSERT(has_item(), "pull::value without an item");
        return std::move(value_);
    }
    /// The failure; precondition: status() == state::failed (Debug asserts).
    const error& failure_reason() const noexcept {
        STEPWRIGHT_ASSERT(is_failed(), "pull::failure_reason without a failure");
        return error_;
    }

private:
    explicit pull(state s) noexcept : state_(s) {}
    pull(std::in_place_t /*tag*/, T&& value) noexcept : state_(state::item) {
        std::construct_at(&value_, std::move(value));
    }
    void destroy() noexcept {
        if (state_ == state::item) {
            std::destroy_at(&value_);
        } else if (state_ == state::failed) {
            std::destroy_at(&error_);
        }
    }

    union {
        T value_;
        error error_;
    };
    state state_;
};

/// Asynchronous pull stream. A single consumer at a time calls
/// try_pull() and on_ready() (never concurrently with each other, and not while
/// its readiness wait is outstanding); producers of concrete sources are
/// thread-safe as documented per type.
///
/// - try_pull() never blocks. `end` and `failed` are sticky: every later call
///   returns the same outcome (the same error).
/// - on_ready(done, token) completes once when try_pull() may make progress (an
///   item, end or failure is available): synchronously if that is already true,
///   otherwise later on any thread (through the hand-off trampoline), or with
///   errc::cancelled if token is cancelled first. At most one readiness wait is
///   outstanding per source (Debug asserts). Readiness may be spurious (try_pull
///   may still return `pending`), which implementations avoid.
/// - Destroying a source with an outstanding readiness wait is a precondition
///   violation (Debug asserts): cancel the wait and let it complete first.
/// Sources have identity: they are neither copyable nor movable.
template <class T>
class source {
public:
    class ready_op;
    /// Item type.
    using value_type = T;

    source(const source&) = delete;
    source& operator=(const source&) = delete;
    /// Destroy only without an outstanding readiness wait.
    virtual ~source() = default;
    /// Non-blocking pull; see the class comment.
    virtual pull<T> try_pull() = 0;
    /// One-shot readiness notification; see the class comment.
    virtual void on_ready(completion done, cancel_token token) = 0;
    /// A task wrapping on_ready(): wait for readiness from a machine with
    /// after<&D::f>(ready_op_member). Restartable, allocation-free; guaranteed
    /// copy elision.
    ready_op ready() noexcept;

protected:
    /// Construct the interface part of a concrete source.
    source() noexcept = default;
};

/// Task completing when its source is ready to make progress (see
/// source::on_ready), or with errc::cancelled when its token is cancelled first.
/// Restartable after completion; the source must outlive it.
template <class T>
class source<T>::ready_op final : public task {
public:
    /// Bind to s (non-owning).
    explicit ready_op(source& s) noexcept : source_(&s) {}

private:
    void on_start(completion done, cancel_token token) override {
        source_->on_ready(done, std::move(token));
    }

    source* source_;
};

template <class T>
typename source<T>::ready_op source<T>::ready() noexcept {
    return ready_op(*this);
}
} // namespace stepwright
