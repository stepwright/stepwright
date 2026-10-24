#pragma once

#include <stepwright/config.hpp>
#include <stepwright/detail/live_state.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>

namespace stepwright::detail {
// Core of the opt-in live task registry (STEPWRIGHT_TRACK_TASKS; public API in
// debug/live_tasks.hpp). Included by machine.hpp and source_machine.hpp only when
// tracking is enabled; always usable by the debug header.

// Intrusive registry node embedded in tracked machines. type/started/thread are
// written before linking, under the registry lock; state and step are updated by
// the owning driver with relaxed atomics and read racily (for diagnostics) by
// snapshots.
struct live_node {
    live_node() noexcept = default;
    live_node(const live_node&) = delete;
    live_node& operator=(const live_node&) = delete;
    ~live_node() = default;

    void set(live_state s, const char* resume) noexcept {
        step.store(resume, std::memory_order_relaxed);
        state.store(s, std::memory_order_relaxed);
    }

    live_node* next = nullptr;
    live_node* prev = nullptr;
    const char* type = nullptr;
    std::chrono::steady_clock::time_point started;
    std::thread::id thread;
    std::atomic<const char*> step{nullptr};
    std::atomic<live_state> state{live_state::running};
    bool linked = false; // guarded by the registry lock
};

// Global list of live nodes under one mutex: O(1) add/remove, no allocation. The
// lock is never held while user code runs. Immortal (constructed in static
// storage, never destroyed), so machines may complete during static destruction.
class live_registry {
public:
    live_registry(const live_registry&) = delete;
    live_registry& operator=(const live_registry&) = delete;
    ~live_registry() = delete;

    static live_registry& instance() noexcept {
        alignas(live_registry) static std::array<std::byte, sizeof(live_registry)> storage;
        static auto* const registry = ::new (static_cast<void*>(storage.data())) live_registry();
        return *registry;
    }
    /// Register node (not linked) as started now on this thread, running.
    void add(live_node& node, const char* type) noexcept {
        node.type = type;
        node.started = std::chrono::steady_clock::now();
        node.thread = std::this_thread::get_id();
        node.set(live_state::running, nullptr);
        const std::lock_guard lock(mutex_);
        STEPWRIGHT_ASSERT(!node.linked, "registering a live task twice");
        node.prev = nullptr;
        node.next = head_;
        if (head_ != nullptr) {
            head_->prev = &node;
        }
        head_ = &node;
        node.linked = true;
    }
    /// Unregister node if it is registered.
    void remove(live_node& node) noexcept {
        const std::lock_guard lock(mutex_);
        if (!node.linked) {
            return;
        }
        if (node.prev != nullptr) {
            node.prev->next = node.next;
        } else {
            head_ = node.next;
        }
        if (node.next != nullptr) {
            node.next->prev = node.prev;
        }
        node.next = nullptr;
        node.prev = nullptr;
        node.linked = false;
    }
    /// Call f(const live_node&) for each node under the lock. Internal: f must be
    /// short and must not run user code or touch the registry.
    template <class F>
    void visit(F&& f) {
        const std::lock_guard lock(mutex_);
        for (const live_node* node = head_; node != nullptr; node = node->next) {
            f(*node);
        }
    }

private:
    live_registry() noexcept = default;

    std::mutex mutex_;
    live_node* head_ = nullptr;
};
} // namespace stepwright::detail
