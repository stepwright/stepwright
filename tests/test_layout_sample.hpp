#pragma once
#include <stepwright/stepwright.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>
struct layout_sample : stepwright::machine<layout_sample> {
    step begin();
};
std::size_t ndebug_size();
std::size_t ndebug_align();

// Every public M2 type, measured in both the Debug-checks and NDEBUG TUs.
using sample_fn = void (*)(const stepwright::completion&, const stepwright::cancel_token&);
#define STEPWRIGHT_M2_LAYOUT_TYPES(X)                                                              \
    X(stepwright::event)                                                                           \
    X(stepwright::event::waiter)                                                                   \
    X(stepwright::shared_task)                                                                     \
    X(stepwright::shared_task::waiter)                                                             \
    X(stepwright::async_mutex)                                                                     \
    X(stepwright::async_mutex::lock_op)                                                            \
    X(stepwright::async_mutex::guarded)                                                            \
    X(stepwright::async_shared_mutex)                                                              \
    X(stepwright::async_shared_mutex::lock_op)                                                     \
    X(stepwright::async_shared_mutex::shared_lock_op)                                              \
    X(stepwright::async_shared_mutex::guarded)                                                     \
    X(stepwright::async_shared_mutex::guarded_shared)                                              \
    X(stepwright::when_all<2>)                                                                     \
    X(stepwright::when_all<8>)                                                                     \
    X(stepwright::when_all_range)                                                                  \
    X(stepwright::when_any<2>)                                                                     \
    X(stepwright::when_any<8>)                                                                     \
    X(stepwright::when_any_range)                                                                  \
    X(stepwright::timer_service::entry)                                                            \
    X(stepwright::steady_timer_service)                                                            \
    X(stepwright::testing::manual_timer_service)                                                   \
    X(stepwright::delay)                                                                           \
    X(stepwright::with_deadline)                                                                   \
    X(stepwright::fn_task<sample_fn>)                                                              \
    X(stepwright::finally_task)                                                                    \
    X(stepwright::reschedule)

#define STEPWRIGHT_M2_LAYOUT_COUNT(T) +1
inline constexpr std::size_t m2_layout_count =
    0 STEPWRIGHT_M2_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_COUNT);
using m2_layout = std::array<std::pair<std::size_t, std::size_t>, m2_layout_count>;
#define STEPWRIGHT_M2_LAYOUT_ENTRY(T) std::pair<std::size_t, std::size_t>{sizeof(T), alignof(T)},
#define STEPWRIGHT_M2_LAYOUT_NAME(T) #T,
inline m2_layout measure_m2_layout() {
    return m2_layout{STEPWRIGHT_M2_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_ENTRY)};
}
m2_layout ndebug_m2_layout();

// Every public M3 type (aliases keep template commas out of the macro).
struct layout_source : stepwright::source_machine<layout_source, int> {
    step begin();
};
using layout_merge2 = stepwright::merge<int, 2>;
using layout_merge8 = stepwright::merge<int, 8>;
using layout_just3 = stepwright::just_source<int, 3>;
using layout_range = stepwright::range_source<std::vector<int>>;
using layout_item_fn = std::optional<int> (*)();
using layout_generate = stepwright::generate_source<layout_item_fn>;
using layout_each_fn = void (*)(int&&);
using layout_for_each = stepwright::for_each_op<int, layout_each_fn>;
#define STEPWRIGHT_M3_LAYOUT_TYPES(X)                                                              \
    X(stepwright::pull<int>)                                                                       \
    X(stepwright::source<int>::ready_op)                                                           \
    X(layout_source)                                                                               \
    X(stepwright::channel<int>)                                                                    \
    X(stepwright::channel<int>::send_op)                                                           \
    X(layout_merge2)                                                                               \
    X(layout_merge8)                                                                               \
    X(stepwright::merge_range<int>)                                                                \
    X(stepwright::broadcast<int>)                                                                  \
    X(stepwright::broadcast<int>::publish_op)                                                      \
    X(stepwright::broadcast<int>::subscription)                                                    \
    X(stepwright::empty_source<int>)                                                               \
    X(layout_just3)                                                                                \
    X(layout_range)                                                                                \
    X(layout_generate)                                                                             \
    X(stepwright::interval_source)                                                                 \
    X(layout_for_each)

inline constexpr std::size_t m3_layout_count =
    0 STEPWRIGHT_M3_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_COUNT);
using m3_layout = std::array<std::pair<std::size_t, std::size_t>, m3_layout_count>;
inline m3_layout measure_m3_layout() {
    return m3_layout{STEPWRIGHT_M3_LAYOUT_TYPES(STEPWRIGHT_M2_LAYOUT_ENTRY)};
}
m3_layout ndebug_m3_layout();
