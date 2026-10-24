#pragma once
#include <stepwright/stepwright.hpp>

#include <array>
#include <cstddef>
#include <utility>
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
