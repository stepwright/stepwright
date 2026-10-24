#pragma once

#include <cstdint>

namespace stepwright::detail {
/// What a machine tracked by the live task registry is doing (see
/// debug/live_tasks.hpp). Declared unconditionally so the engine's tracking hooks
/// compile to nothing when STEPWRIGHT_TRACK_TASKS is off.
enum class live_state : std::uint8_t {
    running,       ///< Steps are executing.
    waiting_child, ///< Waiting for a child started with after() or forward().
    parked,        ///< Waiting for wake().
    deferred,      ///< A deferred step is queued on the home executor.
    suspended      ///< A source that emitted an item and waits for the next pull.
};
} // namespace stepwright::detail
