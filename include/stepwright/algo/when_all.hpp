#pragma once

#include <stepwright/detail/join.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <vector>

namespace stepwright {
/// Structured fan-out over N children. Starts every child with an internal token
/// linked to the start token and completes once *all* children have completed
/// (never earlier). Result: the first child failure in completion order; with
/// when_all_policy::cancel_on_error that failure also requests cancellation of the
/// other children. If the start token was cancelled and no child failed, the
/// result is errc::cancelled. Zero children complete successfully, synchronously.
/// Children may complete synchronously or on any thread. Restartable after
/// completion; children must stay valid and idle at start and outlive the run.
/// Construction allocates the internal cancel source once; runs do not allocate.
template <std::size_t N>
class when_all final : public detail::join_base {
public:
    /// Join the given children (non-owning).
    explicit when_all(std::array<task*, N> children,
                      when_all_policy policy = when_all_policy::cancel_on_error)
        : join_base(all_mode(policy))
        , children_(children) {
        attach(children_, nullptr);
    }
    /// Join children given by reference, with cancel_on_error.
    template <class... T>
        requires(sizeof...(T) == N && (std::derived_from<T, task> && ...))
    explicit when_all(T&... children) : when_all(std::array<task*, N>{&children...}) {}

private:
    std::array<task*, N> children_;
};
template <class... T>
    requires(std::derived_from<T, task> && ...)
when_all(T&...) -> when_all<sizeof...(T)>;

/// when_all over a runtime-sized span of children; the span (and the tasks) must
/// stay valid until the last run completes. Same semantics as when_all<N>. With
/// many children (64 or more) construction also allocates a small fixed set of
/// completion counters, so children completing on many threads do not all update
/// one counter.
class when_all_range final : public detail::join_base {
public:
    /// Join the given children (non-owning).
    explicit when_all_range(std::span<task* const> children,
                            when_all_policy policy = when_all_policy::cancel_on_error)
        : join_base(all_mode(policy))
        , shard_storage_(children.size() >= shard_threshold ? shard_count : 0) {
        attach(children, nullptr, shard_storage_.empty() ? nullptr : shard_storage_.data());
    }

private:
    // Allocated once, at construction (sized 0 or shard_count).
    std::vector<shard> shard_storage_;
};
} // namespace stepwright
