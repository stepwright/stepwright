#pragma once

#include <stepwright/config.hpp>
#include <stepwright/detail/live_registry.hpp>

#include <chrono>
#include <cstddef>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

/// Opt-in registry of in-flight machines, for diagnosing hangs.
///
/// Enabled by the CMake option STEPWRIGHT_TRACK_TASKS (default OFF), which defines
/// STEPWRIGHT_TRACK_TASKS=1 on the `stepwright` interface target. Tracking CHANGES THE
/// LAYOUT of machine<D>, source_machine<D, T>, step and source_step: set it
/// consistently for the whole program (CMake propagates it to every consumer of
/// the target; do not define it per translation unit). With tracking disabled
/// there is no member, no call and no cost, and these functions report nothing.
///
/// A tracked machine<D> is registered from start until just before its completion
/// is delivered; a tracked source_machine<D, T> from its first pull (or readiness
/// check) until it ends, fails or is destroyed. Registration is O(1) under a short
/// lock, through a node embedded in the object (no allocation).
namespace stepwright::debug {
/// Whether this translation unit was compiled with tracking.
inline constexpr bool tracking_enabled = STEPWRIGHT_TRACK_TASKS != 0;

/// What a live task is doing.
using task_state = detail::live_state;

/// Name of a state: "running", "waiting_child", "parked", "deferred", "suspended".
constexpr const char* to_string(task_state state) noexcept {
    switch (state) {
        case task_state::running:
            return "running";
        case task_state::waiting_child:
            return "waiting_child";
        case task_state::parked:
            return "parked";
        case task_state::deferred:
            return "deferred";
        case task_state::suspended:
            return "suspended";
    }
    return "unknown";
}

/// Snapshot of one live machine. Strings have static storage duration.
struct live_task {
    const char* type_name = nullptr;        ///< The machine's type (D).
    task_state state = task_state::running; ///< Momentary driver state.
    /// The step it will resume at (for example "&my_op::on_reply"), or nullptr
    /// while running or when it finishes with a forwarded child.
    const char* resume_step = nullptr;
    std::chrono::steady_clock::time_point started; ///< Start (or first pull) time.
    std::thread::id thread;                        ///< Thread that started it.
};

/// Copy the live entries under the registry lock, then call f(const live_task&)
/// for each one outside the lock (f may start or complete machines). Thread-safe.
/// Allocates the snapshot. Entries change concurrently: a snapshot is a moment.
template <class F>
void for_each_live(F&& f) {
    std::vector<live_task> snapshot;
    detail::live_registry::instance().visit([&](const detail::live_node& node) {
        snapshot.push_back({.type_name = node.type,
                            .state = node.state.load(std::memory_order_relaxed),
                            .resume_step = node.step.load(std::memory_order_relaxed),
                            .started = node.started,
                            .thread = node.thread});
    });
    for (const live_task& task : snapshot) {
        f(task);
    }
}

/// Number of live entries (a snapshot). Thread-safe.
inline std::size_t live_count() {
    std::size_t count = 0;
    detail::live_registry::instance().visit([&](const detail::live_node&) { ++count; });
    return count;
}

/// Print one line per live entry:
/// `<type> state=<state> step=<resume step or -> age=<microseconds>us thread=<id>`.
/// Returns the number of lines. Thread-safe; no lock is held while writing.
inline std::size_t dump(std::ostream& os) {
    const auto now = std::chrono::steady_clock::now();
    std::size_t lines = 0;
    for_each_live([&](const live_task& task) {
        const auto age =
            std::chrono::duration_cast<std::chrono::microseconds>(now - task.started).count();
        os << task.type_name << " state=" << to_string(task.state)
           << " step=" << (task.resume_step != nullptr ? task.resume_step : "-")
           << " age=" << (age > 0 ? age : 0) << "us thread=" << task.thread << '\n';
        ++lines;
    });
    return lines;
}
} // namespace stepwright::debug
