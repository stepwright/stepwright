#pragma once

#include <stepwright/cancel.hpp>
#include <stepwright/completion.hpp>

#include <memory>
#include <utility>

namespace stepwright {
/// Restartable asynchronous operation. Owners serialize starts and keep the task
/// alive until its exactly-once completion; destroying active work is invalid.
/// Implementations must never touch themselves after delivering completion,
/// because receivers can destroy them. A token provides cooperative cancellation.
class task {
public:
    /// Destroy an idle/completed task polymorphically; never implicitly cancel/wait.
    virtual ~task() = default;
    /// A task has stable, unique identity; copying/moving active state is forbidden.
    task(const task&) = delete;
    /// Assignment cannot replace that identity.
    task& operator=(const task&) = delete;
    /// Start only while idle with a nonempty target. Completion occurs exactly
    /// once, before return or later on any thread, and may destroy this task.
    /// May restart from/after completion; concurrent starts are not supported.
    /// Always inlined: a two-line forwarder on every driver step, whose inlining
    /// GCC otherwise drops in larger translation units (see docs/M2-primitives.md).
    STEPWRIGHT_ALWAYS_INLINE void start(completion done, cancel_token token = {}) {
        STEPWRIGHT_ASSERT(done, "start requires a completion");
        on_start(done, std::move(token));
    }

protected:
    /// Construct idle, for derived task implementations.
    task() = default;
    /// Implement start's exactly-once/lifetime contract, propagating the token to
    /// owned work. Call done outside locks and do not access this afterwards.
    virtual void on_start(completion done, cancel_token token) = 0;
};
/// Unique ownership, not an automatic cancel/join handle; destroy only when idle.
using task_ptr = std::unique_ptr<task>;
} // namespace stepwright
