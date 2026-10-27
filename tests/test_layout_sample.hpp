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

// Primitive types, measured in both the Debug-checks and NDEBUG TUs.
using sample_fn = void (*)(const stepwright::completion&, const stepwright::cancel_token&);
#define STEPWRIGHT_PRIMITIVE_LAYOUT_TYPES(X)                                                       \
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
    X(stepwright::sequence<2>)                                                                     \
    X(stepwright::sequence_range)                                                                  \
    X(stepwright::retry_policy)                                                                    \
    X(stepwright::retry)                                                                           \
    X(stepwright::timer_service::entry)                                                            \
    X(stepwright::steady_timer_service)                                                            \
    X(stepwright::testing::manual_timer_service)                                                   \
    X(stepwright::delay)                                                                           \
    X(stepwright::with_deadline)                                                                   \
    X(stepwright::fn_task<sample_fn>)                                                              \
    X(stepwright::finally_task)                                                                    \
    X(stepwright::reschedule)

#define STEPWRIGHT_LAYOUT_COUNT(T) +1
inline constexpr std::size_t primitive_layout_count =
    0 STEPWRIGHT_PRIMITIVE_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_COUNT);
using primitive_layout = std::array<std::pair<std::size_t, std::size_t>, primitive_layout_count>;
#define STEPWRIGHT_LAYOUT_ENTRY(T) std::pair<std::size_t, std::size_t>{sizeof(T), alignof(T)},
#define STEPWRIGHT_LAYOUT_NAME(T) #T,
inline primitive_layout measure_primitive_layout() {
    return primitive_layout{STEPWRIGHT_PRIMITIVE_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_ENTRY)};
}
primitive_layout ndebug_primitive_layout();

// Stream types (aliases keep template commas out of the macro).
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
#define STEPWRIGHT_STREAM_LAYOUT_TYPES(X)                                                          \
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

inline constexpr std::size_t stream_layout_count =
    0 STEPWRIGHT_STREAM_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_COUNT);
using stream_layout = std::array<std::pair<std::size_t, std::size_t>, stream_layout_count>;
inline stream_layout measure_stream_layout() {
    return stream_layout{STEPWRIGHT_STREAM_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_ENTRY)};
}
stream_layout ndebug_stream_layout();

// Runtime types.
#define STEPWRIGHT_RUNTIME_LAYOUT_TYPES(X)                                                         \
    X(stepwright::thread_pool)                                                                     \
    X(stepwright::testing::virtual_executor)                                                       \
    X(stepwright::testing::schedule)

inline constexpr std::size_t runtime_layout_count =
    0 STEPWRIGHT_RUNTIME_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_COUNT);
using runtime_layout = std::array<std::pair<std::size_t, std::size_t>, runtime_layout_count>;
inline runtime_layout measure_runtime_layout() {
    return runtime_layout{STEPWRIGHT_RUNTIME_LAYOUT_TYPES(STEPWRIGHT_LAYOUT_ENTRY)};
}
runtime_layout ndebug_runtime_layout();
