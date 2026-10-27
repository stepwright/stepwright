#pragma once

#include <stepwright/detail/join.hpp>
#include <stepwright/detail/task_range.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <vector>

namespace stepwright {
/// First-wins fan-out over N children. Starts every child with an internal token
/// linked to the start token; the first child to complete wins and requests
/// cancellation of the others. Completes once *all* children have completed, with
/// the winner's result. Zero children is a precondition violation (Debug asserts;
/// without STEPWRIGHT_DEBUG it completes successfully with winner() == npos).
/// Children may complete synchronously or on any thread. Restartable after
/// completion; children must stay valid and idle at start and outlive the run.
/// Construction allocates the internal cancel source once; runs do not allocate.
template <std::size_t N>
class when_any final : public detail::join_base {
public:
    /// Race the given children (non-owning).
    explicit when_any(std::array<task*, N> children) : join_base(mode::any), children_(children) {
        attach(children_, slots_.data());
    }
    /// Race children given by reference.
    template <class... T>
        requires(sizeof...(T) == N && (std::derived_from<T, task> && ...))
    explicit when_any(T&... children) : when_any(std::array<task*, N>{&children...}) {}
    /// Index of the first child to complete; valid after completion.
    std::size_t winner() const noexcept { return winner_index(); }

private:
    std::array<task*, N> children_;
    std::array<slot, N> slots_{};
};
template <class... T>
    requires(std::derived_from<T, task> && ...)
when_any(T&...) -> when_any<sizeof...(T)>;

/// when_any over a runtime-sized span of children. Per-child completion contexts
/// are allocated once at construction, sized from the span; the span (and tasks)
/// must stay valid until the last run completes. Same semantics as when_any<N>.
class when_any_range final : public detail::join_base {
public:
    /// Race the given children (non-owning).
    explicit when_any_range(std::span<task* const> children)
        : join_base(mode::any)
        , children_(children)
        , slots_(children.size()) {
        attach(children, slots_.data());
    }
    template <detail::task_object_range R>
    /// Copy addresses from a mutable sized/multipass lvalue range (one extra allocation).
    /// Elements must keep stable addresses for this join's lifetime. Runs allocate
    /// nothing; winner and structured cancellation semantics are unchanged.
    explicit when_any_range(R& children)
        : join_base(mode::any)
        , children_(children)
        , slots_(children_.view().size()) {
        attach(children_.view(), slots_.data());
    }
    template <detail::task_object_range R>
        requires(!std::is_lvalue_reference_v<R>)
    /// Rejected: temporary task storage would dangle.
    explicit when_any_range(R&& children) = delete;
    /// Index of the first child to complete; valid after completion.
    std::size_t winner() const noexcept { return winner_index(); }

private:
    detail::task_range children_;
    std::vector<slot> slots_;
};
} // namespace stepwright
