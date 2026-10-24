#pragma once

#include <stepwright/config.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

namespace stepwright {
class cancel_registration;
namespace detail {
struct cancel_invocation {
    cancel_registration* node;
    std::thread::id thread = std::this_thread::get_id();
};
struct cancel_state {
    std::atomic<std::size_t> refs{1};
    std::atomic<bool> requested{false};
    std::mutex mutex;
    std::condition_variable changed;
    cancel_registration* head = nullptr;
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
        token_ = token;
        fn_ = fn;
        ctx_ = ctx;
        auto* state = token_.state_;
        if (!state) {
            return;
        }
        cancel_token keep_alive = token_;
        std::unique_lock lock(state->mutex);
        if (!state->requested.load(std::memory_order_relaxed)) {
            link_.next = state->head;
            previous_ = &state->head;
            if (link_.next) {
                link_.next->previous_ = &link_.next;
            }
            state->head = this;
            return;
        }
        invoke(state, this, lock);
    }
    /// O(1) unlink. Wait for a callback running on another thread; self-disarm
    /// returns immediately. Retains the token, and may be called repeatedly.
    void disarm() noexcept {
        auto* state = token_.state_;
        if (!state) {
            return;
        }
        std::unique_lock lock(state->mutex);
        if (previous_) {
            unlink();
            return;
        }
        for (;;) {
            auto* found = link_.invocation;
            if (!found) {
                return;
            }
            if (found->thread == std::this_thread::get_id()) {
                found->node = nullptr;
                link_.invocation = nullptr;
                return;
            }
            state->changed.wait(lock);
        }
    }
    /// Borrow the token (retained even after disarm); valid until rearm/destruction.
    const cancel_token& token() const noexcept { return token_; }

private:
    friend class cancel_source;
    // Under the state mutex: previous_ != nullptr selects link_.next;
    // otherwise link_.invocation is active. Linked nodes cannot be running.
    void unlink() noexcept {
        *previous_ = link_.next;
        if (link_.next) {
            link_.next->previous_ = previous_;
        }
        previous_ = nullptr;
        link_.invocation = nullptr;
    }
    static void invoke(detail::cancel_state* state, cancel_registration* node,
                       std::unique_lock<std::mutex>& lock) noexcept {
        const auto fn = node->fn_;
        void* ctx = node->ctx_;
        detail::cancel_invocation call{.node = node, .thread = std::this_thread::get_id()};
        node->link_.invocation = &call;
        lock.unlock();
        fn(ctx);
        lock.lock();
        if (call.node) {
            call.node->link_.invocation = nullptr;
        }
        state->changed.notify_all();
        // Self-disarm clears call.node before the node can be destroyed/rearmed.
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
    /// Request once and invoke registered callbacks on this thread, without the
    /// mutex held. Returns true only for the first request. A callback may destroy
    /// this handle; local ownership keeps dispatch state alive until return.
    bool request_cancel() noexcept {
        cancel_token keep_alive = token_;
        auto* state = keep_alive.state_;
        if (!state) {
            return false;
        }
        std::unique_lock lock(state->mutex);
        if (state->requested.exchange(true, std::memory_order_seq_cst)) {
            return false;
        }
        while (state->head) {
            auto* node = state->head;
            node->unlink();
            cancel_registration::invoke(state, node, lock);
        }
        return true;
    }
    /// Clear a previous request so the same state can serve another run without
    /// allocation. Precondition (asserted in part): no registration is armed on the
    /// state, no request_cancel() is in progress, and every task that observed a
    /// token from this source has completed. Stale token copies observe the reset.
    /// Not thread-safe with other uses of this state; moved-from sources do nothing.
    void reset() noexcept {
        auto* state = token_.state_;
        if (!state) {
            return;
        }
        const std::lock_guard lock(state->mutex);
        STEPWRIGHT_ASSERT(state->head == nullptr, "cancel_source reset with armed registrations");
        state->requested.store(false, std::memory_order_seq_cst);
    }

private:
    cancel_token token_;
};

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
