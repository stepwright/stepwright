#pragma once

#include <stepwright/config.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace stepwright {
/// Abstract timer service. Entries are intrusive nodes embedded in the timer user,
/// so scheduling never allocates. Implementations are thread-safe and invoke each
/// due entry's fire callback exactly once, outside their internal locks.
class timer_service {
public:
    /// All services measure deadlines on the steady clock's time line.
    using clock = std::chrono::steady_clock;
    struct entry;

    /// Services have identity: entries refer to them while scheduled.
    timer_service() = default;
    timer_service(const timer_service&) = delete;
    timer_service& operator=(const timer_service&) = delete;
    /// Destroy only when no entry is scheduled or firing.
    virtual ~timer_service() = default;
    /// The service's current time; thread-safe.
    virtual clock::time_point now() const noexcept = 0;
    /// Schedule an idle (not scheduled) entry to fire at deadline. Entries with
    /// equal deadlines fire in scheduling order. The entry must stay alive and
    /// untouched until it fired or was cancelled. Thread-safe; no allocation.
    virtual void schedule(entry& e, clock::time_point deadline) noexcept = 0;
    /// true: removed before firing, the callback will not run. false: the
    /// callback has run or is running (or e was never scheduled). When the callback
    /// is running on another thread, cancel waits until it has returned, so after
    /// cancel() returns the entry may be reused or destroyed. Called from inside
    /// the entry's own callback (same thread) it returns false without waiting.
    virtual bool cancel(entry& e) noexcept = 0;
};

/// Intrusive timer node. Set fire before scheduling; the remaining fields are
/// reserved for timer_service implementations and must not be touched while the
/// entry is scheduled.
struct timer_service::entry {
    /// Called once when due, on the service's firing thread, outside locks. It may
    /// destroy or reschedule the entry; the service does not touch it afterwards.
    void (*fire)(entry*) noexcept = nullptr;

    // Implementation-owned: pairing-heap links, ordering key and state.
    clock::time_point deadline; ///< Service-owned ordering key.
    std::uint64_t sequence = 0; ///< Service-owned FIFO tie-break.
    entry* child = nullptr;     ///< Service-owned heap/ready-list linkage.
    entry* sibling = nullptr;   ///< Service-owned heap/ready-list linkage.
    entry* prev = nullptr;      ///< Parent (first child) or previous sibling.
    bool scheduled = false;     ///< Service-owned pending flag.
};

namespace detail {
// Deadline-ordered intrusive pairing heap plus records of callbacks currently
// running. Insert is O(1); pop and arbitrary removal are O(log n) amortized.
// Ties are broken by a sequence number, giving FIFO order for equal deadlines.
// Not synchronized: the owning service locks around every call.
class timer_queue {
public:
    using entry = timer_service::entry;
    using time_point = timer_service::clock::time_point;

    // Stack-resident record of an entry whose callback is running.
    struct firing {
        entry* target = nullptr;
        std::thread::id thread;
        firing* next = nullptr;
    };
    enum class cancel_result : std::uint8_t {
        removed,          ///< Taken out of the heap; will not fire.
        firing_elsewhere, ///< Callback running on another thread: wait and retry.
        not_pending       ///< Fired already, firing on this thread, or never scheduled.
    };

    bool empty() const noexcept { return root_ == nullptr; }
    std::size_t size() const noexcept { return size_; }
    entry* top() const noexcept { return root_; }

    void schedule(entry& e, time_point deadline) noexcept {
        STEPWRIGHT_ASSERT(!e.scheduled, "scheduling an already scheduled timer entry");
        STEPWRIGHT_ASSERT(e.fire, "timer entry without fire callback");
        e.deadline = deadline;
        e.sequence = next_sequence_++;
        e.child = nullptr;
        e.sibling = nullptr;
        e.prev = nullptr;
        e.scheduled = true;
        root_ = root_ ? meld(root_, &e) : &e;
        ++size_;
    }
    cancel_result status(const entry& e) const noexcept {
        return e.scheduled ? cancel_result::removed : running_state(e);
    }
    void remove(entry& e) noexcept {
        if (&e == root_) {
            pop();
            return;
        }
        if (e.prev->child == &e) {
            e.prev->child = e.sibling;
        } else {
            e.prev->sibling = e.sibling;
        }
        if (e.sibling) {
            e.sibling->prev = e.prev;
        }
        entry* rest = merge_pairs(e.child);
        if (rest) {
            root_ = meld(root_, rest);
        }
        clear(e);
        --size_;
    }
    /// Pop the earliest entry if due, recording it as firing on this thread. The
    /// record lives on the firing thread's stack and is unlinked by fired() before
    /// that frame ends (GCC cannot prove this across the unlock/callback/lock).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
    entry* pop_due(time_point now, firing& record) noexcept {
        entry* e = root_;
        if (!e || e->deadline > now) {
            return nullptr;
        }
        pop();
        record.target = e;
        record.thread = std::this_thread::get_id();
        record.next = firing_;
        firing_ = &record;
        return e;
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    void fired(firing& record) noexcept {
        for (firing** link = &firing_; *link; link = &(*link)->next) {
            if (*link == &record) {
                *link = record.next;
                return;
            }
        }
    }

private:
    cancel_result running_state(const entry& e) const noexcept {
        for (const firing* record = firing_; record; record = record->next) {
            if (record->target == &e) {
                return record->thread == std::this_thread::get_id()
                           ? cancel_result::not_pending
                           : cancel_result::firing_elsewhere;
            }
        }
        return cancel_result::not_pending;
    }
    static bool before(const entry* a, const entry* b) noexcept {
        return a->deadline < b->deadline ||
               (a->deadline == b->deadline && a->sequence < b->sequence);
    }
    static void clear(entry& e) noexcept {
        e.child = nullptr;
        e.sibling = nullptr;
        e.prev = nullptr;
        e.scheduled = false;
    }
    // Link two detached roots; the later one becomes the first child of the other.
    static entry* meld(entry* a, entry* b) noexcept {
        if (before(b, a)) {
            entry* swap = a;
            a = b;
            b = swap;
        }
        b->prev = a;
        b->sibling = a->child;
        if (a->child) {
            a->child->prev = b;
        }
        a->child = b;
        a->sibling = nullptr;
        a->prev = nullptr;
        return a;
    }
    // Standard two-pass pairing of a sibling list, iteratively (no recursion).
    static entry* merge_pairs(entry* first) noexcept {
        entry* paired = nullptr; // reversed list of pair roots, linked by sibling
        while (first) {
            entry* a = first;
            entry* b = a->sibling;
            first = b ? b->sibling : nullptr;
            a->sibling = nullptr;
            a->prev = nullptr;
            entry* merged = a;
            if (b) {
                b->sibling = nullptr;
                b->prev = nullptr;
                merged = meld(a, b);
            }
            merged->sibling = paired;
            paired = merged;
        }
        entry* result = paired;
        if (result) {
            paired = result->sibling;
            result->sibling = nullptr;
        }
        while (paired) {
            entry* next = paired->sibling;
            paired->sibling = nullptr;
            result = meld(result, paired);
            paired = next;
        }
        return result;
    }
    void pop() noexcept {
        entry* e = root_;
        root_ = merge_pairs(e->child);
        clear(*e);
        --size_;
    }

    entry* root_ = nullptr;
    std::size_t size_ = 0;
    std::uint64_t next_sequence_ = 0;
    firing* firing_ = nullptr;
};
} // namespace detail

/// Timer service with one background thread on std::chrono::steady_clock.
/// Callbacks run on that thread and must be short (typically: deliver a
/// completion through the hand-off trampoline). Thread-safe. Destruction stops
/// and joins the thread; entries still scheduled then never fire (Debug asserts
/// that none are scheduled). Construction starts a thread and may throw.
/// After observing a callback's effects, an owner on another thread may destroy
/// the service even if schedule() has not returned. No other timers or calls may
/// remain outstanding; now() alone is not a destruction signal.
class steady_timer_service final : public timer_service {
public:
    /// Start the timer thread.
    steady_timer_service() : thread_([this] { run(); }) {}
    steady_timer_service(const steady_timer_service&) = delete;
    steady_timer_service& operator=(const steady_timer_service&) = delete;
    /// Stop and join the timer thread; not from a timer callback.
    ~steady_timer_service() override {
        {
            const std::lock_guard lock(mutex_);
            STEPWRIGHT_ASSERT(queue_.empty(),
                              "destroying a steady_timer_service with scheduled timers");
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    /// steady_clock::now(); thread-safe.
    clock::time_point now() const noexcept override { return clock::now(); }
    /// See timer_service::schedule. Wakes the thread only if e is the new earliest.
    void schedule(entry& e, clock::time_point deadline) noexcept override {
        const std::lock_guard lock(mutex_);
        queue_.schedule(e, deadline);
        if (queue_.top() == &e) {
            // The worker cannot fire e before this call's final mutex unlock.
            // Notifying after unlocking would let the callback's observer destroy
            // wake_ while this thread was still notifying it.
            wake_.notify_one();
        }
    }
    /// See timer_service::cancel; blocks while e's callback runs on the timer thread.
    bool cancel(entry& e) noexcept override {
        std::unique_lock lock(mutex_);
        for (;;) {
            switch (queue_.status(e)) {
                case detail::timer_queue::cancel_result::removed:
                    queue_.remove(e);
                    return true;
                case detail::timer_queue::cancel_result::not_pending:
                    return false;
                case detail::timer_queue::cancel_result::firing_elsewhere:
                    ++cancel_waiters_;
                    idle_.wait(lock);
                    --cancel_waiters_;
                    break;
            }
        }
    }

private:
    void run() noexcept {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            const entry* next = queue_.top();
            if (!next) {
                wake_.wait(lock);
                continue;
            }
            if (next->deadline > clock::now()) {
                // Copy: wait_until reads its argument after waking, when the entry
                // may already have been cancelled and destroyed.
                const clock::time_point deadline = next->deadline;
                wake_.wait_until(lock, deadline);
                continue;
            }
            detail::timer_queue::firing record;
            entry* due = queue_.pop_due(clock::now(), record);
            if (!due) {
                continue;
            }
            lock.unlock();
            due->fire(due);
            lock.lock();
            queue_.fired(record);
            if (cancel_waiters_ != 0) {
                idle_.notify_all();
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    detail::timer_queue queue_;
    std::size_t cancel_waiters_ = 0;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace stepwright
