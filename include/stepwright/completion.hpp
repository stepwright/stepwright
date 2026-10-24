#pragma once

#include <stepwright/config.hpp>
#include <stepwright/error.hpp>

namespace stepwright {
/// Non-owning, allocation-free completion target. Copy freely, but do not mutate
/// a target while invoking that same object. ctx and its receiver must outlive the
/// call; result is borrowed only for the call. Receivers may destroy the task and
/// must copy error to retain it. The task contract permits exactly one invocation.
struct completion {
    /// Callback ABI; exceptions must not escape.
    using fn_type = void (*)(void*, const error&) noexcept;
    fn_type fn = nullptr; ///< Null denotes an unbound target.
    void* ctx = nullptr;  ///< Borrowed context, passed unchanged to fn.
    /// Whether a target is bound; safe concurrently with const access.
    explicit operator bool() const noexcept { return fn != nullptr; }
    /// Invoke a bound target on this thread (precondition: fn). No member access
    /// follows the call, so the receiver may destroy the containing object.
    void operator()(const error& result) const noexcept {
        STEPWRIGHT_ASSERT(fn, "empty completion");
        const auto call = fn;
        void* context = ctx;
        call(context, result);
    }
    template <auto MemFn, class C>
    /// Bind void C::f(const error&) noexcept, borrowing obj until invocation.
    /// No allocation; obj's own thread-safety determines allowed calling threads.
    static completion bind(C* obj) noexcept {
        return {
            .fn = [](void* context,
                     const error& result) noexcept { (static_cast<C*>(context)->*MemFn)(result); },
            .ctx = obj};
    }
};
} // namespace stepwright
