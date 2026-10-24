#pragma once

#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/executor.hpp>
#include <stepwright/timer.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace stepwright::testing {
/// How virtual_executor picks the next ready item.
enum class schedule : std::uint8_t {
    fifo,  ///< Posting order (timers by the moment they became due).
    random ///< Uniform seeded choice among all ready items and due timers.
};

/// Deterministic single-threaded executor and timer service on virtual time, for
/// reproducible concurrency tests (design: docs/M4-runtime.md).
///
/// - post(), schedule() and cancel() are thread-safe, but items and timer
///   callbacks run only inside the run_* calls, on the calling thread. Do not call
///   run_* concurrently from several threads; nested calls from items are allowed.
/// - Virtual time starts at clock::time_point{} and moves only in run_for() and
///   run_until_quiescent(), and only when nothing is ready: it jumps to the next
///   deadline, and every timer due then becomes a ready item. A deadline at or
///   before now() is ready at once.
/// - schedule::fifo runs items in posting order and due timers in order of
///   becoming due, interleaved by that moment. schedule::random draws the next item
///   uniformly from all ready items and due timers with a splitmix64 generator
///   seeded by the constructor: the same seed and the same single-threaded program
///   give exactly the same execution order; different seeds explore different
///   interleavings. Report seed() when a test fails.
/// - Nothing is allocated, ever (items and timer entries are intrusive).
/// - Destroy only without scheduled or due timers (Debug asserts); queued items
///   are simply not run.
class virtual_executor final : public executor, public timer_service {
public:
    /// Also expose the allocating callable convenience overload.
    using executor::post;

    /// Create with a scheduling policy and (for schedule::random) a seed.
    explicit virtual_executor(testing::schedule policy = testing::schedule::fifo,
                              std::uint64_t seed = 0) noexcept
        : policy_(policy)
        , seed_(seed)
        , random_(seed) {}
    virtual_executor(const virtual_executor&) = delete;
    virtual_executor& operator=(const virtual_executor&) = delete;
    /// Debug asserts that no timer is scheduled or due.
    ~virtual_executor() override {
        STEPWRIGHT_ASSERT(timers_.empty() && due_head_ == nullptr,
                          "destroying a virtual_executor with scheduled timers");
    }

    // --- executor ---------------------------------------------------------------

    /// Append item to the ready set; thread-safe, no allocation, never runs inline.
    void post(work_item& item) noexcept override {
        item.next = nullptr;
        const std::lock_guard lock(lock_);
        if (tail_ != nullptr) {
            tail_->next = &item;
        } else {
            head_ = &item;
        }
        tail_ = &item;
        ++ready_count_;
        ++posted_total_;
    }
    /// True only inside run_* on the thread running them (including nesting).
    bool running_in_this_thread() const noexcept override {
        for (const scope* s = active_scope; s != nullptr; s = s->previous) {
            if (s->owner == this) {
                return true;
            }
        }
        return false;
    }

    // --- timer_service (virtual time) ---------------------------------------------

    /// The virtual time.
    clock::time_point now() const noexcept override {
        const std::lock_guard lock(lock_);
        return now_;
    }
    /// See timer_service::schedule. Due at once if deadline <= now().
    void schedule(entry& e, clock::time_point deadline) noexcept override {
        STEPWRIGHT_ASSERT(e.child != due_marker(), "scheduling an already due timer entry");
        const std::lock_guard lock(lock_);
        if (deadline <= now_) {
            e.deadline = deadline;
            make_due(e);
        } else {
            timers_.schedule(e, deadline);
        }
    }
    /// See timer_service::cancel: true if removed before its callback ran (scheduled
    /// or due); waits while the callback runs on another thread.
    bool cancel(entry& e) noexcept override {
        std::unique_lock lock(lock_);
        for (;;) {
            if (e.scheduled) {
                timers_.remove(e);
                return true;
            }
            if (e.child == due_marker()) {
                unlink_due(e);
                return true;
            }
            const firing* record = firing_;
            while (record != nullptr && record->target != &e) {
                record = record->next;
            }
            if (record == nullptr || record->thread == std::this_thread::get_id()) {
                return false;
            }
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
    }

    // --- driving ------------------------------------------------------------------

    /// Run one ready item or due timer chosen by the policy; false if none is ready.
    /// Time does not move.
    bool run_one() {
        work_item* item = nullptr;
        entry* timer = nullptr;
        firing record;
        {
            const std::lock_guard lock(lock_);
            const std::size_t total = ready_count_ + due_count_;
            if (total == 0) {
                return false;
            }
            bool take_item = false;
            std::size_t index = 0;
            if (policy_ == testing::schedule::fifo) {
                // Run the head item first iff it was posted before the head timer
                // became due (due timers are stamped with posted_total_).
                take_item = ready_count_ != 0 &&
                            (due_count_ == 0 || posted_total_ - ready_count_ < due_head_->sequence);
            } else {
                index = total == 1 ? 0 : static_cast<std::size_t>(next_random() % total);
                take_item = index < ready_count_;
                if (!take_item) {
                    index -= ready_count_;
                }
            }
            if (take_item) {
                item = take_item_at(index);
            } else {
                timer = due_head_;
                for (std::size_t i = 0; i < index; ++i) {
                    timer = timer->sibling;
                }
                unlink_due(*timer);
                record.target = timer;
                record.thread = std::this_thread::get_id();
                record.next = firing_;
                firing_ = &record;
            }
        }
        scope current{.owner = this, .previous = active_scope};
        active_scope = &current;
        if (item != nullptr) {
            item->run(item);
        } else {
            timer->fire(timer);
        }
        active_scope = current.previous;
        if (timer != nullptr) {
            const std::lock_guard lock(lock_);
            for (firing** link = &firing_; *link != nullptr; link = &(*link)->next) {
                if (*link == &record) {
                    *link = record.next;
                    break;
                }
            }
        }
        return true;
    }
    /// Run until nothing is ready (time does not move); returns the number run.
    std::size_t run_until_idle() {
        std::size_t count = 0;
        while (run_one()) {
            ++count;
        }
        return count;
    }
    /// Run, advancing virtual time to each deadline up to now() + d (when nothing is
    /// ready), then set the time to now() + d. Returns the number of items run.
    std::size_t run_for(clock::duration d) {
        const clock::time_point limit = now() + d;
        std::size_t count = 0;
        for (;;) {
            count += run_until_idle();
            if (!advance(limit)) {
                break;
            }
        }
        const std::lock_guard lock(lock_);
        if (now_ < limit) {
            now_ = limit;
        }
        return count;
    }
    /// Run and advance time until nothing is ready and no timer is scheduled.
    /// Returns the number of items run. Does not terminate for a program that keeps
    /// rescheduling timers forever.
    std::size_t run_until_quiescent() {
        std::size_t count = 0;
        for (;;) {
            count += run_until_idle();
            if (!advance(clock::time_point::max())) {
                return count;
            }
        }
    }
    /// Number of ready items: posted items plus due timers.
    std::size_t pending() const noexcept {
        const std::lock_guard lock(lock_);
        return ready_count_ + due_count_;
    }
    /// Number of timers scheduled for the future (not yet due).
    std::size_t scheduled_timers() const noexcept {
        const std::lock_guard lock(lock_);
        return timers_.size();
    }
    /// The seed given to the constructor.
    std::uint64_t seed() const noexcept { return seed_; }

private:
    struct scope {
        const virtual_executor* owner;
        scope* previous;
    };
    // Stack-resident record of a timer callback in progress (for cancel()).
    struct firing {
        entry* target = nullptr;
        std::thread::id thread;
        firing* next = nullptr;
    };

    // Due entries are linked through `sibling`; `child` marks membership and
    // `sequence` holds the number of items posted before the entry became due.
    static entry* due_marker() noexcept {
        static entry marker;
        return &marker;
    }
    void make_due(entry& e) noexcept {
        e.scheduled = false;
        e.child = due_marker();
        e.sibling = nullptr;
        e.prev = nullptr;
        e.sequence = posted_total_;
        if (due_tail_ != nullptr) {
            due_tail_->sibling = &e;
        } else {
            due_head_ = &e;
        }
        due_tail_ = &e;
        ++due_count_;
    }
    void unlink_due(entry& e) noexcept {
        entry* previous = nullptr;
        for (entry* cursor = due_head_; cursor != &e; cursor = cursor->sibling) {
            previous = cursor;
        }
        if (previous != nullptr) {
            previous->sibling = e.sibling;
        } else {
            due_head_ = e.sibling;
        }
        if (due_tail_ == &e) {
            due_tail_ = previous;
        }
        e.child = nullptr;
        e.sibling = nullptr;
        --due_count_;
    }
    work_item* take_item_at(std::size_t index) noexcept {
        work_item* previous = nullptr;
        work_item* item = head_;
        for (std::size_t i = 0; i < index; ++i) {
            previous = item;
            item = item->next;
        }
        if (previous != nullptr) {
            previous->next = item->next;
        } else {
            head_ = item->next;
        }
        if (tail_ == item) {
            tail_ = previous;
        }
        --ready_count_;
        return item;
    }
    // If the earliest timer is due by limit, move time to it and make every entry
    // due at that instant ready (deadline, then scheduling order).
    bool advance(clock::time_point limit) noexcept {
        const std::lock_guard lock(lock_);
        const entry* next = timers_.top();
        if (next == nullptr || next->deadline > limit) {
            return false;
        }
        if (next->deadline > now_) {
            now_ = next->deadline;
        }
        detail::timer_queue::firing record;
        while (entry* e = timers_.pop_due(now_, record)) {
            timers_.fired(record);
            make_due(*e);
        }
        return true;
    }
    // splitmix64 (Steele, Lea and Flood, OOPSLA 2014).
    std::uint64_t next_random() noexcept {
        random_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = random_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    inline static thread_local scope* active_scope = nullptr;

    mutable detail::spin_lock lock_;
    work_item* head_ = nullptr;
    work_item* tail_ = nullptr;
    std::size_t ready_count_ = 0;
    std::uint64_t posted_total_ = 0;
    entry* due_head_ = nullptr;
    entry* due_tail_ = nullptr;
    std::size_t due_count_ = 0;
    detail::timer_queue timers_;
    clock::time_point now_;
    firing* firing_ = nullptr;
    testing::schedule policy_;
    std::uint64_t seed_;
    std::uint64_t random_;
};
} // namespace stepwright::testing
