#pragma once

#include <stepwright/detail/task_range.hpp>
#include <stepwright/machine.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>

namespace stepwright {
namespace detail {
// The machine's driver absorbs synchronous child completions; no recursive
// start/on_child chain, even for arbitrarily large ranges in Debug builds.
class sequence_core : public machine<sequence_core> {
protected:
    sequence_core() noexcept = default;
    void attach(std::span<task* const> children) noexcept { children_ = children; }

private:
    friend class machine<sequence_core>;
    step begin() noexcept {
        next_ = 0;
        return then<&sequence_core::next>();
    }
    step next() noexcept {
        if (next_ == children_.size()) {
            return done();
        }
        if (cancel_requested()) {
            return fail(errc::cancelled);
        }
        // A no-argument continuation propagates the first child failure without
        // starting any later child. Every started child finishes before we do.
        return after<&sequence_core::next>(*children_[next_++]);
    }
    std::span<task* const> children_;
    std::size_t next_ = 0;
};
} // namespace detail

/// Run N borrowed children in order with the start token. Stop on first failure
/// or cancellation between children; an empty sequence succeeds. Restartable,
/// constant stack, no allocation. Children must remain alive through all runs.
template <std::size_t N>
class sequence final : public detail::sequence_core {
public:
    /// Store the child addresses inline.
    explicit sequence(std::array<task*, N> children) noexcept : children_(children) {
        attach(children_);
    }
    /// Sequence task references, with no allocation.
    template <class... T>
        requires(sizeof...(T) == N && (std::derived_from<T, task> && ...))
    explicit sequence(T&... children) noexcept : sequence(std::array<task*, N>{&children...}) {}

private:
    std::array<task*, N> children_;
};
template <class... T>
    requires(std::derived_from<T, task> && ...)
sequence(T&...) -> sequence<sizeof...(T)>;

/// Runtime-sized sequence. A span is borrowed unchanged; a sized/multipass lvalue
/// range of task objects has its addresses copied once (one allocation). Tasks
/// must keep stable addresses for this sequence's lifetime; runs allocate nothing.
class sequence_range final : public detail::sequence_core {
public:
    /// Borrow the span storage and its tasks through every run.
    explicit sequence_range(std::span<task* const> children) noexcept : children_(children) {
        attach(children_.view());
    }
    template <detail::task_object_range R>
    /// Copy addresses, not task objects, from a stable lvalue range.
    explicit sequence_range(R& children) : children_(children) {
        attach(children_.view());
    }
    template <detail::task_object_range R>
        requires(!std::is_lvalue_reference_v<R>)
    /// Rejected: temporary task storage would dangle.
    explicit sequence_range(R&& children) = delete;

private:
    detail::task_range children_;
};
} // namespace stepwright
