#pragma once

#include <stepwright/detail/wait_core.hpp>
#include <stepwright/task.hpp>

#include <utility>

namespace stepwright::detail {
// Task base of the primitive waiters (event/shared_task waiters, mutex lock operations,
// delay). The completion protocol itself lives in wait_core (see there): the
// waiter is published before its cancellation registration is armed, one phase
// word decides whether the resolver or the starter delivers, and delivery goes
// through the hand-off trampoline.
class async_op : public task {
public:
    async_op(const async_op&) = delete;
    async_op& operator=(const async_op&) = delete;
    /// Destroy only while idle (not started, or completed); Debug asserts.
    ~async_op() override = default;

protected:
    async_op() noexcept = default;

    /// Record the receiver before the operation is published.
    void prepare(completion done, const cancel_token& token) noexcept {
        core_.prepare(done, token);
    }
    /// Arm cancellation after publication; the last access by the starter. With an
    /// empty token this does not touch the object (it may already be complete).
    void arm(const cancel_token& token, void (*on_cancel)(void*) noexcept) noexcept {
        if (!token.can_be_cancelled()) {
            return;
        }
        core_.arm(token, on_cancel, this);
    }
    /// Claim the single resolution right among racing resolvers (timer vs cancel).
    bool decide() noexcept { return core_.decide(); }
    /// Store the result; complete if the starter has finished arming.
    void resolve(error result) noexcept { core_.resolve(std::move(result)); }

private:
    wait_core core_;
};
} // namespace stepwright::detail
