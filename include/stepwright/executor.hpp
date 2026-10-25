#pragma once

#include <stepwright/config.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Intrusive stable-address work node. Do not queue twice simultaneously; keep
/// alive through invocation. Its callback may destroy it: executors do not touch
/// the node after run returns. Posting transfers next's ownership until dequeue.
struct work_item {
    void (*run)(work_item*) noexcept = nullptr; ///< Required callback, outside executor locks.
    work_item* next = nullptr; ///< Executor-owned queue linkage; caller-owned while not queued.
};

/// Non-owning scheduling interface. Concrete implementations define concurrency;
/// keep an executor alive until all outstanding work has run. No internal lock
/// may be held while invoking work. Callbacks must not throw.
class executor {
public:
    /// Polymorphic destruction; pending work must already be drained by the owner.
    virtual ~executor() = default;
    /// Queue/run a stable, nonqueued node without allocation; it must outlive run.
    virtual void post(work_item& item) noexcept = 0;
    /// Query whether this thread is executing here; conservative default is false.
    /// An executor that may run an item inside post() must return true on that
    /// thread (machines rely on it to keep deferral chains flat).
    virtual bool running_in_this_thread() const noexcept { return false; }
    /// Convenience post: allocate one owning node for F and free it after calling.
    /// Captures must survive invocation; allocation can throw, invocation must not.
    template <class F>
        requires(!std::is_base_of_v<work_item, std::remove_cvref_t<F>>)
    void post(F&& fn) {
        struct owned_item : work_item {
            std::decay_t<F> fn;
            explicit owned_item(F&& function) : fn(std::forward<F>(function)) {
                run = [](work_item* item) noexcept {
                    std::unique_ptr<owned_item> owned(static_cast<owned_item*>(item));
                    owned->fn();
                };
            }
        };
        post(*new owned_item(std::forward<F>(fn)));
    }
};

/// Executes immediately on the caller, on any thread. Concurrent posts do not
/// serialize user work; no queued nodes survive post().
class inline_executor final : public executor {
public:
    /// Also expose the allocating callable convenience overload.
    using executor::post;
    /// Invoke now; never access the node after its callback.
    void post(work_item& item) noexcept override { item.run(&item); }
    /// Always true: every caller is an inline-executor execution thread.
    bool running_in_this_thread() const noexcept override { return true; }
};

/// Thread-safe intrusive FIFO. Producers may post concurrently; run_one/run_all
/// may be called by consumers on any thread, and do not globally serialize work.
/// Drain the queue before destruction; queued nodes/executor must remain alive.
class manual_executor final : public executor {
    struct execution_scope {
        const manual_executor* owner;
        execution_scope* previous;
    };

public:
    /// Also expose the allocating callable convenience overload.
    using executor::post;
    /// O(1) thread-safe enqueue with no allocation; item must not already be queued.
    void post(work_item& item) noexcept override {
        item.next = nullptr;
        std::lock_guard lock(mutex_);
        if (tail_) {
            tail_->next = &item;
        } else {
            head_ = &item;
        }
        tail_ = &item;
        ++count_;
    }
    /// True only inside this thread's run_one/run_all callbacks, including nesting.
    bool running_in_this_thread() const noexcept override {
        for (auto* scope = current; scope; scope = scope->previous) {
            if (scope->owner == this) {
                return true;
            }
        }
        return false;
    }
    /// Dequeue the oldest node, unlock and invoke it; false if empty. Other
    /// consumers may run concurrently, so FIFO dequeue need not mean finish order.
    bool run_one() {
        work_item* item = nullptr;
        {
            std::lock_guard lock(mutex_);
            item = head_;
            if (!item) {
                return false;
            }
            head_ = item->next;
            if (!head_) {
                tail_ = nullptr;
            }
            --count_;
        }
        execution_scope scope{.owner = this, .previous = current};
        current = &scope;
        item->run(item);
        current = scope.previous;
        return true;
    }
    /// Run until empty, including newly posted items; return number run. Concurrent
    /// producers can keep this running indefinitely. No lock spans invocation.
    std::size_t run_all() {
        std::size_t count = 0;
        while (run_one()) {
            ++count;
        }
        return count;
    }
    /// Thread-safe instantaneous queued count (excludes executing callbacks).
    std::size_t pending() const noexcept {
        std::lock_guard lock(mutex_);
        return count_;
    }

private:
    inline static thread_local execution_scope* current = nullptr;
    mutable std::mutex mutex_;
    work_item* head_ = nullptr;
    work_item* tail_ = nullptr;
    std::size_t count_ = 0;
};
} // namespace stepwright
