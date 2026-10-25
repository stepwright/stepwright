#pragma once

#include <stepwright/config.hpp>
#include <stepwright/detail/spin_lock.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace stepwright {
class cancel_registration;
class cancel_source;
namespace detail {
struct cancel_source_access;
struct cancel_invocation {
    cancel_registration* node;
    std::thread::id thread = std::this_thread::get_id();
};
// Registrations are spread over independently locked stripes (chosen by node
// address), so threads arming and disarming on one shared token do not serialize
// on one lock or one cache line. Design and measurements: docs/design/performance.md.
inline constexpr std::size_t cancel_stripes = 8;
struct alignas(cache_line) cancel_stripe {
    spin_lock lock;
    // Whether the list may be non-empty: written (seq_cst) under the lock when a
    // node is linked into it or the last one leaves; read without the lock by a
    // requester, which skips empty stripes.
    std::atomic<bool> occupied{false};
    cancel_registration* head = nullptr; // guarded by lock
};
struct cancel_state {
    // Written by every token copy, so kept apart from what registrations touch.
    alignas(cache_line) std::atomic<std::size_t> refs{1};
    alignas(cache_line) std::atomic<bool> requested{false};
    // A foreign disarm waiting for a running callback counts itself in `waiters`
    // (under the node's stripe lock) and sleeps on `finished`, which the requester
    // bumps (under the same lock) after a callback returns, when anyone waits.
    std::atomic<std::uint32_t> waiters{0};
    std::atomic<std::uint32_t> finished{0};
    std::array<cancel_stripe, cancel_stripes> stripes{};
};
} // namespace detail

/// Shared cancellation observation handle. Distinct copies may be used on any
/// thread; serialize changes to the same handle. Empty handles perform no atomics.
class cancel_token {
public:
    /// Construct an empty, never-cancelled token without allocation.
    cancel_token() noexcept = default;
    /// Share the state; source remains usable. Safe against requests on that state.
    cancel_token(const cancel_token& other) noexcept : state_(other.state_) {
        if (state_) {
            state_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    /// Transfer the state, leaving other empty; serialize access to both handles.
    cancel_token(cancel_token&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
    /// Share another handle's state; not concurrent with changes to either handle.
    cancel_token& operator=(const cancel_token& other) noexcept {
        cancel_token copy(other);
        std::swap(state_, copy.state_);
        return *this;
    }
    /// Transfer state, leaving other empty; not concurrent with either handle's use.
    cancel_token& operator=(cancel_token&& other) noexcept {
        cancel_token moved(std::move(other));
        std::swap(state_, moved.state_);
        return *this;
    }
    /// Release this reference; other copies/registrations keep the state alive.
    STEPWRIGHT_ALWAYS_INLINE ~cancel_token() {
        if (state_) {
            release();
        }
    }
    /// Whether state exists (not whether it is already requested); concurrent query.
    bool can_be_cancelled() const noexcept { return state_ != nullptr; }
    /// Sequentially consistent query for a nonempty token, supporting the engine's
    /// park/cancel handshake. Safe concurrently with request_cancel(); empty tokens
    /// still perform no atomic operations.
    bool cancel_requested() const noexcept {
        return state_ && state_->requested.load(std::memory_order_seq_cst);
    }

private:
    void release() noexcept {
        if (state_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete state_;
        }
    }
    friend class cancel_source;
    friend class cancel_registration;
    detail::cancel_state* state_ = nullptr;
};

/// Allocation-free intrusive engine registration. Owns a token reference; arm,
/// disarm and destruction of this node must be serialized by its owner, except
/// that disarm/destruction safely synchronize with a concurrently running callback.
class cancel_registration {
public:
    /// Construct an unarmed node. Storage must remain stable while registered.
    cancel_registration() noexcept = default;
    /// Nodes cannot be copied/moved: their address is part of the intrusive list.
    cancel_registration(const cancel_registration&) = delete;
    /// Assignment cannot transfer a node that may be registered or executing.
    cancel_registration& operator=(const cancel_registration&) = delete;
    /// Disarm, waiting for foreign callbacks; self-destruction never waits.
    ~cancel_registration() { disarm(); }

    /// Replace any registration with fn(ctx); no allocation. fn/ctx must remain
    /// valid until disarm returns, or until self-disarm abandons their lifetime.
    /// Already-requested tokens invoke inline here; empty tokens do nothing.
    /// Invocations are outside the lock; escaping callback exceptions terminate.
    void arm(const cancel_token& token, void (*fn)(void*) noexcept, void* ctx) {
        disarm();
        bind(token, fn, ctx);
        activate();
    }
    /// Replace the token and callback of an *idle* registration (never activated
    /// since the previous bind, or disarmed since) without registering: token()
    /// returns the new token, but requests run nothing until activate(). Touches no
    /// shared state beyond the token's reference count (Debug checks idleness).
    void bind(cancel_token token, void (*fn)(void*) noexcept, void* ctx) noexcept {
#if STEPWRIGHT_DEBUG
        if (auto* state = token_.state_) {
            auto& stripe = stripe_of(*state);
            stripe.lock.lock();
            const bool idle = previous_ == nullptr && link_.invocation == nullptr;
            stripe.lock.unlock();
            STEPWRIGHT_ASSERT(idle, "cancel_registration rebound while armed");
        }
#endif
        token_ = std::move(token);
        fn_ = fn;
        ctx_ = ctx;
    }
    /// Register the bound callback (at most once per bind): O(1), or an inline
    /// invocation if the token is already requested; empty tokens do nothing.
    void activate() noexcept {
        auto* state = token_.state_;
        if (!state) {
            return;
        }
        auto& stripe = stripe_of(*state);
        stripe.lock.lock();
        STEPWRIGHT_ASSERT(previous_ == nullptr && link_.invocation == nullptr,
                          "cancel_registration activated twice");
        // Link, then look at the flag. With the requester's exchange and its look
        // at `occupied` (all seq_cst) either the requester finds this node or this
        // load sees the request; under the lock exactly one side unlinks and runs it.
        link(stripe);
        if (!state->requested.load(std::memory_order_seq_cst)) {
            stripe.lock.unlock();
            return;
        }
        unlink(stripe);
        // The callback may destroy this node, and with it the token reference.
        const cancel_token keep_alive = token_;
        invoke(*state, stripe, this);
    }
    /// O(1) unlink. Wait for a callback running on another thread; self-disarm
    /// returns immediately. Retains the token, and may be called repeatedly.
    /// Always inlined: with an empty token (the common case for waiters) it is one
    /// test.
    STEPWRIGHT_ALWAYS_INLINE void disarm() noexcept {
        if (auto* state = token_.state_) {
            disarm_registered(*state);
        }
    }
    /// Borrow the token (retained even after disarm); valid until rearm/destruction.
    const cancel_token& token() const noexcept { return token_; }

private:
    friend class cancel_source;
    void disarm_registered(detail::cancel_state& state_ref) noexcept {
        auto* state = &state_ref;
        auto& stripe = stripe_of(*state);
        stripe.lock.lock();
        if (previous_) {
            unlink(stripe);
            stripe.lock.unlock();
            return;
        }
        for (;;) {
            auto* found = link_.invocation;
            if (!found) {
                break;
            }
            if (found->thread == std::this_thread::get_id()) {
                found->node = nullptr;
                link_.invocation = nullptr;
                break;
            }
            state->waiters.fetch_add(1, std::memory_order_relaxed);
            const auto seen = state->finished.load(std::memory_order_relaxed);
            stripe.lock.unlock();
            state->finished.wait(seen, std::memory_order_acquire);
            stripe.lock.lock();
            state->waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        stripe.lock.unlock();
    }
    detail::cancel_stripe& stripe_of(detail::cancel_state& state) const noexcept {
        // Fibonacci hashing of the address: neighbouring nodes use different stripes.
        constexpr int shift = 64 - 3;
        static_assert(detail::cancel_stripes == std::size_t{1} << (64 - shift));
        const auto bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));
        return state.stripes[static_cast<std::size_t>((bits * 0x9E3779B97F4A7C15ULL) >> shift)];
    }
    // Under the stripe lock: previous_ != nullptr selects link_.next; otherwise
    // link_.invocation is active. Linked nodes cannot be running.
    void link(detail::cancel_stripe& stripe) noexcept {
        link_.next = stripe.head;
        previous_ = &stripe.head;
        if (link_.next) {
            link_.next->previous_ = &link_.next;
        }
        stripe.head = this;
        stripe.occupied.store(true, std::memory_order_seq_cst);
    }
    void unlink(detail::cancel_stripe& stripe) noexcept {
        *previous_ = link_.next;
        if (link_.next) {
            link_.next->previous_ = previous_;
        }
        previous_ = nullptr;
        link_.invocation = nullptr;
        if (stripe.head == nullptr) {
            stripe.occupied.store(false, std::memory_order_seq_cst);
        }
    }
    // Called with the stripe lock held and node unlinked; returns with it released.
    // The caller keeps state alive (the callback may destroy the node).
    static void invoke(detail::cancel_state& state, detail::cancel_stripe& stripe,
                       cancel_registration* node) noexcept {
        const auto fn = node->fn_;
        void* ctx = node->ctx_;
        detail::cancel_invocation call{.node = node, .thread = std::this_thread::get_id()};
        node->link_.invocation = &call;
        stripe.lock.unlock();
        fn(ctx);
        stripe.lock.lock();
        // Self-disarm clears call.node before the node can be destroyed/rearmed.
        if (call.node) {
            call.node->link_.invocation = nullptr;
        }
        const bool waited = state.waiters.load(std::memory_order_relaxed) != 0;
        if (waited) {
            state.finished.fetch_add(1, std::memory_order_release);
        }
        stripe.lock.unlock();
        if (waited) {
            state.finished.notify_all();
        }
    }
    cancel_token token_;
    union links {
        cancel_registration* next;
        detail::cancel_invocation* invocation = nullptr;
    } link_;
    cancel_registration** previous_ = nullptr;
    void (*fn_)(void*) noexcept = nullptr;
    void* ctx_ = nullptr;
};

/// One-allocation shared cancellation source. Copies share state, moves leave
/// an inert source. Requests/queries on stable handles are thread-safe; serialize
/// assignment/destruction of a handle with its other uses. Tokens outlive sources.
class cancel_source {
public:
    /// Create a fresh not-requested state (one allocation).
    cancel_source() { token_.state_ = new detail::cancel_state; }
    /// Obtain an owning token copy; safe concurrently with requests.
    cancel_token token() const noexcept { return token_; }
    /// Concurrent SC query of the request flag; moved-from means false, no atomics.
    bool cancel_requested() const noexcept { return token_.cancel_requested(); }
    /// Request once and invoke registered callbacks on this thread, without any
    /// lock held. Returns true only for the first request. A callback may destroy
    /// this handle; local ownership keeps dispatch state alive until return.
    bool request_cancel() noexcept {
        cancel_token keep_alive = token_;
        auto* state = keep_alive.state_;
        if (!state) {
            return false;
        }
        if (state->requested.exchange(true, std::memory_order_seq_cst)) {
            return false;
        }
        // A registration that this look at `occupied` misses sees the request
        // itself and runs inline (see activate and docs/design/performance.md).
        for (auto& stripe : state->stripes) {
            if (!stripe.occupied.load(std::memory_order_seq_cst)) {
                continue;
            }
            stripe.lock.lock();
            while (auto* node = stripe.head) {
                node->unlink(stripe);
                cancel_registration::invoke(*state, stripe, node);
                stripe.lock.lock();
            }
            stripe.lock.unlock();
        }
        return true;
    }

private:
    // Cancellation is one-way for users. Only the library's own combinators, which
    // own their sources, may rearm one (through detail::cancel_source_access).
    friend struct detail::cancel_source_access;
    // Clear a previous request so the same state can serve another run without
    // allocation. Precondition (asserted in part): no registration is armed on the
    // state, no request_cancel() is in progress, and every task that observed a
    // token from this source has completed. Stale token copies observe the reset.
    // Not thread-safe with other uses of this state; moved-from sources do nothing.
    void reset() noexcept {
        auto* state = token_.state_;
        if (!state) {
            return;
        }
#if STEPWRIGHT_DEBUG
        for (auto& stripe : state->stripes) {
            stripe.lock.lock();
            const bool empty = stripe.head == nullptr;
            stripe.lock.unlock();
            STEPWRIGHT_ASSERT(empty, "cancel_source reset with armed registrations");
        }
#endif
        state->requested.store(false, std::memory_order_seq_cst);
    }

    cancel_token token_;
};

namespace detail {
// Internal route for combinators that own a cancel_source and reuse it across
// runs (see cancel_source::reset). Not part of the public API.
struct cancel_source_access {
    static void reset(cancel_source& source) noexcept { source.reset(); }
};
} // namespace detail

template <class F>
/// RAII, allocation-free registration owning its callable. F() runs at most once
/// on the requester (or constructor thread if already requested), outside locks.
/// F must not throw; it may destroy this registration provided it makes no further
/// access to the destroyed callable. Empty tokens are no-ops.
class cancel_callback {
public:
    /// Register a callable; it may run before construction returns.
    cancel_callback(const cancel_token& token, F fn) : fn_(std::move(fn)) {
        registration_.arm(
            token, [](void* ctx) noexcept { static_cast<cancel_callback*>(ctx)->fn_(); }, this);
    }
    /// Non-copyable/non-movable because registration storage is intrusive.
    cancel_callback(const cancel_callback&) = delete;
    /// Assignment cannot transfer stable registration storage.
    cancel_callback& operator=(const cancel_callback&) = delete;
    /// Deregister before destroying F; waits for foreign invocations, not self.
    ~cancel_callback() = default;

private:
    F fn_;
    cancel_registration registration_;
};
} // namespace stepwright
