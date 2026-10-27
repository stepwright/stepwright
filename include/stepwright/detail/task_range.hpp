#pragma once

#include <stepwright/task.hpp>

#include <concepts>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

namespace stepwright::detail {
// Only mutable task objects, not pointer ranges, const objects or proxy values.
// Sized or multipass ranges permit exactly one allocation for copied addresses.
template <class R>
concept task_object_range =
    std::ranges::input_range<R> && (std::ranges::sized_range<R> || std::ranges::forward_range<R>) &&
    std::is_lvalue_reference_v<std::ranges::range_reference_t<R>> &&
    std::derived_from<std::ranges::range_value_t<R>, task> &&
    std::convertible_to<std::ranges::range_reference_t<R>, task&>;

// Span constructors continue borrowing; object-range constructors own only the
// addresses, never the tasks. The containing task has stable identity too.
class task_range {
public:
    explicit task_range(std::span<task* const> children) noexcept : view_(children) {}
    template <task_object_range R>
    explicit task_range(R& children)
        : owned_(static_cast<std::size_t>(std::ranges::distance(children))) {
        std::size_t i = 0;
        for (auto& child : children) {
            owned_[i++] = std::addressof(child);
        }
        view_ = owned_;
    }
    std::span<task* const> view() const noexcept { return view_; }

private:
    std::vector<task*> owned_;
    std::span<task* const> view_;
};
} // namespace stepwright::detail
