#pragma once

#include <stepwright/config.hpp>
#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace stepwright {
namespace detail {
// Padding unit for per-worker state (see cache_line).
inline constexpr std::size_t pool_line = cache_line;
} // namespace detail

/// Fixed-size multi-threaded executor with per-worker queues and work stealing
/// (design and proofs: docs/M4-runtime.md).
///
/// - post() is thread-safe, never allocates (work items are intrusive) and may be
///   called from inside work running on the pool. Posts from a pool thread go to
///   that worker's own queue, which it runs in FIFO order; other posts go to a
///   shared injection queue. Idle workers take injected work and steal from other
///   workers. A worker queue holds 256 items; on overflow its older half moves to
///   the injection queue (where any worker, including the owner, takes it).
/// - Idle workers spin briefly, then sleep. Posting while every worker is busy
///   costs no system call; a post wakes a sleeper only when no worker is searching
///   for work and there is work to spare (a worker's single queued item is its own
///   next item and does not wake anyone).
/// - Items run outside every internal lock and are not touched after run returns.
///   An item must not block waiting for work it posted to the same pool.
/// - shutdown() (also run by the destructor) stops accepting outside work, runs
///   everything already posted, including items posted by those items, and joins
///   the workers. Posting from outside after shutdown has started is a
///   precondition violation (Debug asserts; in Release the item runs only if a
///   worker is still draining, otherwise never). Never shut down or destroy the
///   pool from one of its own threads.
/// - Machines bound to the pool (bind(pool)) continue on it when completions
///   arrive from other threads, and continue inline when delivered by a worker.
class thread_pool final : public executor {
    static constexpr std::uint64_t ring_capacity = 256;

    struct alignas(detail::pool_line) worker {
        // Consumer index: advanced by CAS by the owner's pops and by thieves.
        alignas(detail::pool_line) std::atomic<std::uint64_t> head{0};
        // Written by the owner, read by thieves on every look.
        alignas(detail::pool_line) std::atomic<std::uint64_t> tail{0};
        std::atomic<std::uint64_t> completed{0}; // single writer: this worker
        // Owner-private (posted is also read by wait_idle()): kept off the lines
        // that thieves poll, so a push does not have to win them back.
        alignas(detail::pool_line) std::atomic<std::uint64_t> posted{0}; // single writer
        // A lower bound of head (head only grows): the owner rereads head only
        // when the ring looks full, or learns it from its own pops.
        std::uint64_t head_seen = 0;
        thread_pool* pool = nullptr;
        std::uint64_t random = 0;
        std::uint32_t ticks = 0;
        // Thief-side memory of a victim holding a single item: steal it only if the
        // victim has completed nothing since this was recorded.
        const worker* candidate = nullptr;
        std::uint64_t candidate_completed = 0;
        std::thread thread;
        // Sleep state.
        alignas(detail::pool_line) std::atomic<std::uint32_t> park{0}; // 1 while asleep
        bool in_stack = false; // on the sleeper stack (guarded by sleep_mutex_)
        // Ring slots [head, tail) modulo ring_capacity hold the queued items.
        alignas(detail::pool_line) std::array<std::atomic<work_item*>, ring_capacity> slots{};
    };

public:
    /// Also expose the allocating callable convenience overload.
    using executor::post;

    /// Start threads workers (at least one). Allocates the worker table once.
    /// Throws if a thread cannot be started (already started workers are joined).
    explicit thread_pool(std::size_t threads = std::thread::hardware_concurrency())
        : count_(threads == 0 ? 1 : threads)
        , workers_(count_)
        , sleepers_(count_) {
        for (std::size_t i = 0; i < count_; ++i) {
            workers_[i].pool = this;
            workers_[i].random = (i + 1) * 0x9E3779B97F4A7C15ULL;
        }
        try {
            for (std::size_t i = 0; i < count_; ++i) {
                workers_[i].thread = std::thread([this, i] { run_worker(workers_[i]); });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    /// shutdown(): run all posted work, then join. Not from a pool thread.
    ~thread_pool() override { shutdown(); }

    /// Queue item (stable, not already queued) to run once on a worker. Thread-safe,
    /// no allocation, never runs item inline.
    void post(work_item& item) noexcept override {
        worker* self = current_worker;
        if (self != nullptr && self->pool == this) {
            // A single queued item is the poster's own next item: it runs as soon as
            // the current one returns, so only spare work is worth a wake-up.
            if (push_local(*self, item) >= 2) {
                wake_if_needed();
            }
        } else {
            STEPWRIGHT_ASSERT(!stopping_.load(std::memory_order_relaxed),
                              "post to a thread_pool after shutdown started");
            item.next = nullptr;
            inject(item, item, 1, true);
            wake_if_needed();
        }
    }
    /// True only on this pool's worker threads.
    bool running_in_this_thread() const noexcept override {
        const worker* self = current_worker;
        return self != nullptr && self->pool == this;
    }
    /// Number of worker threads.
    std::size_t size() const noexcept { return count_; }
    /// Block until every item posted before the call (and everything those items
    /// post) has finished running and nothing is queued. Must not be called from a
    /// pool thread (Debug asserts). Work posted concurrently from other threads may
    /// or may not be waited for.
    void wait_idle() {
        STEPWRIGHT_ASSERT(!running_in_this_thread(), "wait_idle from a thread_pool thread");
        std::unique_lock lock(sleep_mutex_);
        while (!idle_snapshot()) {
            ++idle_waiters_;
            idle_cv_.wait(lock);
            --idle_waiters_;
        }
    }
    /// Stop accepting outside work, run everything already posted (including items
    /// posted by running items), then join the workers. Idempotent; a concurrent
    /// call waits for the first. Must not be called from a pool thread.
    void shutdown() noexcept {
        STEPWRIGHT_ASSERT(!running_in_this_thread(), "thread_pool shut down from its own thread");
        const std::lock_guard guard(shutdown_mutex_);
        if (joined_) {
            return;
        }
        {
            const std::lock_guard lock(sleep_mutex_);
            stopping_.store(true, std::memory_order_release);
            while (sleeper_count_ != 0) {
                release_sleeper(*sleepers_[--sleeper_count_]);
            }
        }
        for (std::size_t i = 0; i < count_; ++i) {
            workers_[i].park.notify_one();
        }
        for (std::size_t i = 0; i < count_; ++i) {
            if (workers_[i].thread.joinable()) {
                workers_[i].thread.join();
            }
        }
        joined_ = true;
    }

private:
    static constexpr std::uint64_t searching_one = 1;
    static constexpr std::uint64_t sleeping_one = std::uint64_t{1} << 32;
    static constexpr std::uint64_t searching_mask = sleeping_one - 1;
    static constexpr unsigned spin_rounds = 10; // cpu_relax rounds: 1, 2, 4, ... 512
    static constexpr unsigned yield_rounds = 10;
    static constexpr std::uint64_t max_batch = 32;
    static constexpr std::uint32_t fairness_period = 61;

    // --- worker rings -------------------------------------------------------------
    //
    // Single producer (the owner, at tail), multiple consumers (the owner and
    // thieves, by CAS on head). A consumer reads slots [h, h + n) and then claims
    // them with CAS(head, h, h + n): the owner overwrites a slot only when the ring
    // has room relative to the current head, so if the CAS succeeds the slots read
    // were not overwritten. Indices are 64-bit and never wrap (no ABA).

    static std::uint64_t ring_index(std::uint64_t i) noexcept { return i % ring_capacity; }
    // Owner only: append without counting (the items were counted when posted).
    // Precondition: the ring has room.
    static void ring_push(worker& self, work_item* item) noexcept {
        const auto t = self.tail.load(std::memory_order_relaxed);
        self.slots[ring_index(t)].store(item, std::memory_order_relaxed);
        self.tail.store(t + 1, std::memory_order_release);
    }
    static std::uint64_t ring_free(const worker& self) noexcept {
        return ring_capacity - (self.tail.load(std::memory_order_relaxed) -
                                self.head.load(std::memory_order_acquire));
    }
    // Owner only. Returns the number of queued items after the push. The post is
    // counted before the item is published: a thief could otherwise run it, and
    // wait_idle() count its completion, before its post.
    std::uint64_t push_local(worker& self, work_item& item) noexcept {
        self.posted.store(self.posted.load(std::memory_order_relaxed) + 1,
                          std::memory_order_release);
        for (;;) {
            const auto t = self.tail.load(std::memory_order_relaxed);
            // head_seen <= head, so the room check is conservative. Every claim up
            // to it was observed with acquire ordering (below, or by a pop's CAS),
            // so the slots it frees are no longer being read.
            auto h = self.head_seen;
            if (t - h >= ring_capacity) {
                h = self.head.load(std::memory_order_acquire);
                self.head_seen = h;
                if (t - h >= ring_capacity) {
                    overflow(self, h);
                    continue;
                }
            }
            self.slots[ring_index(t)].store(&item, std::memory_order_relaxed);
            self.tail.store(t + 1, std::memory_order_release);
            // At least the number queued (more if thieves took some unseen).
            return t + 1 - h;
        }
    }
    // Owner only, ring full: claim its older half and move it to the injection
    // queue, where every worker (the owner included) can take it.
    void overflow(worker& self, std::uint64_t h) noexcept {
        constexpr std::uint64_t half = ring_capacity / 2;
        if (!self.head.compare_exchange_strong(h, h + half, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            return; // a consumer took something: there is room now
        }
        work_item* first = self.slots[ring_index(h)].load(std::memory_order_relaxed);
        work_item* last = first;
        for (std::uint64_t i = 1; i < half; ++i) {
            work_item* next = self.slots[ring_index(h + i)].load(std::memory_order_relaxed);
            last->next = next;
            last = next;
        }
        last->next = nullptr;
        inject(*first, *last, half, false);
        wake_if_needed();
    }
    // Owner only.
    static work_item* pop_local(worker& self) noexcept {
        auto h = self.head.load(std::memory_order_acquire);
        for (;;) {
            if (h == self.tail.load(std::memory_order_relaxed)) {
                self.head_seen = h;
                return nullptr;
            }
            work_item* item = self.slots[ring_index(h)].load(std::memory_order_relaxed);
            if (self.head.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                self.head_seen = h + 1;
                return item;
            }
        }
    }

    // --- injection queue ---------------------------------------------------------------

    // Append the chain first..last (n items, last->next == nullptr). count: outside
    // posts are counted here; overflowed items were counted by their worker.
    void inject(work_item& first, work_item& last, std::uint64_t n, bool count) noexcept {
        inject_lock_.lock();
        if (inject_tail_ != nullptr) {
            inject_tail_->next = &first;
        } else {
            inject_head_ = &first;
        }
        inject_tail_ = &last;
        inject_size_.store(inject_size_.load(std::memory_order_relaxed) + n,
                           std::memory_order_relaxed);
        if (count) {
            injected_.store(injected_.load(std::memory_order_relaxed) + n,
                            std::memory_order_release);
        }
        inject_lock_.unlock();
    }
    // Take the oldest injected item plus a batch for the own ring. force: lock even
    // if the size hint says empty (the complete scans).
    // announce: call wake_reliably() for items moved into the own ring (false for a
    // searcher, whose stop_searching() hands over, and for the pre-sleep scan).
    work_item* take_injected(worker& self, bool force, bool announce) noexcept {
        if (!force && inject_size_.load(std::memory_order_relaxed) == 0) {
            return nullptr;
        }
        const std::uint64_t room = ring_free(self);
        inject_lock_.lock();
        work_item* first = inject_head_;
        if (first == nullptr) {
            inject_lock_.unlock();
            return nullptr;
        }
        const std::uint64_t available = inject_size_.load(std::memory_order_relaxed);
        const std::uint64_t extra = std::min({available / count_, max_batch, available - 1, room});
        work_item* last = first;
        for (std::uint64_t i = 0; i < extra; ++i) {
            last = last->next;
        }
        inject_head_ = last->next;
        if (inject_head_ == nullptr) {
            inject_tail_ = nullptr;
        }
        const bool more_injected = inject_head_ != nullptr;
        inject_size_.store(available - extra - 1, std::memory_order_relaxed);
        inject_lock_.unlock();
        if (extra != 0) {
            // Count, rather than compare with last->next: once pushed, an item may be
            // stolen, run and re-posted (relinked) before this loop ends.
            work_item* item = first->next;
            for (std::uint64_t i = 0; i < extra; ++i) {
                work_item* next = item->next;
                ring_push(self, item);
                item = next;
            }
            if (announce) {
                wake_reliably();
            }
        } else if (more_injected) {
            // One taker per wake-up: hand the rest of the injected work on.
            wake_if_needed();
        }
        return first;
    }

    // --- stealing ------------------------------------------------------------------

    // Take the older half (at most max_batch) of a victim's ring: run the first item,
    // keep the rest in the own ring (which is empty when stealing is attempted).
    // eager: also take a victim's single item at the first look (see steal_from).
    work_item* steal(worker& self, bool eager, bool announce) noexcept {
        if (count_ == 1) {
            return nullptr;
        }
        self.random ^= self.random << 13U;
        self.random ^= self.random >> 7U;
        self.random ^= self.random << 17U;
        const auto start = static_cast<std::size_t>(self.random % count_);
        for (std::size_t k = 0; k < count_; ++k) {
            worker& victim = workers_[(start + k) % count_];
            if (&victim == &self) {
                continue;
            }
            if (work_item* item = steal_from(self, victim, eager, announce)) {
                return item;
            }
        }
        return nullptr;
    }
    work_item* steal_from(worker& self, worker& victim, bool eager, bool announce) noexcept {
        auto h = victim.head.load(std::memory_order_acquire);
        for (;;) {
            const auto t = victim.tail.load(std::memory_order_acquire);
            const std::uint64_t available = t - h;
            if (available == 0 || available > ring_capacity) {
                return nullptr; // empty (or a stale head racing a wrap: skip)
            }
            if (available == 1 && !eager) {
                // Usually the victim's own next item, popped within nanoseconds:
                // taking it would just bounce work between workers. Take it only if
                // the victim completed nothing since the previous look (it is stuck
                // in a long item). A worker about to sleep looks eagerly instead.
                const auto completed = victim.completed.load(std::memory_order_relaxed);
                if (self.candidate != &victim || self.candidate_completed != completed) {
                    self.candidate = &victim;
                    self.candidate_completed = completed;
                    return nullptr;
                }
            }
            const std::uint64_t n = std::min({(available + 1) / 2, max_batch, ring_free(self) + 1});
            work_item* first = victim.slots[ring_index(h)].load(std::memory_order_relaxed);
            const auto own = self.tail.load(std::memory_order_relaxed);
            for (std::uint64_t i = 1; i < n; ++i) {
                self.slots[ring_index(own + i - 1)].store(
                    victim.slots[ring_index(h + i)].load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
            if (victim.head.compare_exchange_weak(h, h + n, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                if (n > 1) {
                    self.tail.store(own + n - 1, std::memory_order_release);
                    if (announce) {
                        wake_reliably();
                    }
                }
                return first;
            }
        }
    }
    // searching: called by a searcher (see take_injected's announce).
    work_item* find(worker& self, bool searching) noexcept {
        if (++self.ticks == fairness_period) {
            self.ticks = 0;
            if (work_item* item = take_injected(self, false, !searching)) {
                return item;
            }
        }
        if (work_item* item = pop_local(self)) {
            return item;
        }
        if (work_item* item = take_injected(self, false, !searching)) {
            return item;
        }
        return steal(self, false, !searching);
    }
    // Complete scan before sleeping or exiting: the injection queue is checked
    // under its lock regardless of its size hint (the no-lost-wake-up recheck), and
    // a victim's single item is taken at once. A worker about to sleep has nothing
    // better to do, and otherwise that item could wait behind a long-running (or
    // blocked) item while every other worker sleeps: nobody would look twice.
    work_item* scan(worker& self) noexcept {
        if (work_item* item = pop_local(self)) {
            return item;
        }
        if (work_item* item = take_injected(self, true, false)) {
            return item;
        }
        return steal(self, true, false);
    }

    // --- idle protocol ----------------------------------------------------------

    void wake_if_needed() noexcept {
        const auto state = idle_.load(std::memory_order_acquire);
        if ((state >> 32U) != 0 && (state & searching_mask) == 0) {
            wake_one();
        }
    }
    // For items a worker moved into its own ring from elsewhere (an injection
    // batch, stolen extras). It did not post them, so it may block in an item that
    // waits for them: another worker must be able to take them. A searcher hands
    // its search over in stop_searching(); any other worker calls this. A load of
    // idle_ could miss a worker going to sleep whose pre-sleep scan misses the
    // items (store buffering between the ring's tail and idle_); an RMW cannot:
    // either it follows that worker's sleep registration in idle_'s order and
    // sees it, or the registration reads from it and the scan sees the items.
    void wake_reliably() noexcept {
        if (count_ == 1) {
            return; // nobody else could take them
        }
        const auto state = idle_.fetch_add(0, std::memory_order_acq_rel);
        if ((state >> 32U) != 0 && (state & searching_mask) == 0) {
            wake_one();
        }
    }
    bool start_searching() noexcept {
        auto state = idle_.load(std::memory_order_relaxed);
        for (;;) {
            // At most a quarter of the workers search at once (at least one may):
            // every look at a victim costs its owner a cache-line transfer on its
            // next push or pop, and one searcher suffices to ramp up (each one that
            // finds work hands the search over to a sleeper).
            if ((state & searching_mask) != 0 && 4 * (state & searching_mask) >= count_) {
                return false;
            }
            if (idle_.compare_exchange_weak(state, state + searching_one, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                return true;
            }
        }
    }
    void stop_searching(bool found) noexcept {
        const auto old = idle_.fetch_sub(searching_one, std::memory_order_acq_rel);
        // The last searcher found work: hand the search over to a sleeper.
        if (found && (old & searching_mask) == 1 && (old >> 32U) != 0) {
            wake_one();
        }
    }
    // Under sleep_mutex_: take w off the stack and make it a searcher.
    void release_sleeper(worker& w) noexcept {
        w.in_stack = false;
        idle_.fetch_add(searching_one - sleeping_one, std::memory_order_acq_rel);
        w.park.store(0, std::memory_order_release);
    }
    void wake_one() noexcept {
        worker* target = nullptr;
        {
            const std::lock_guard lock(sleep_mutex_);
            if (sleeper_count_ == 0) {
                return;
            }
            target = sleepers_[--sleeper_count_];
            release_sleeper(*target);
        }
        target->park.notify_one();
    }
    work_item* search(worker& self) noexcept {
        for (unsigned round = 0; round < spin_rounds + yield_rounds; ++round) {
            if (work_item* item = find(self, true)) {
                return item;
            }
            if (stopping_.load(std::memory_order_acquire)) {
                return nullptr;
            }
            if (round < spin_rounds) {
                for (unsigned i = 0; i < (1U << round); ++i) {
                    detail::cpu_relax();
                }
            } else {
                std::this_thread::yield();
            }
        }
        return nullptr;
    }
    // No work found: search, then sleep. Returns work, or nullptr to exit.
    work_item* idle(worker& self) noexcept {
        bool searching = start_searching();
        for (;;) {
            if (searching) {
                if (work_item* item = search(self)) {
                    // Hands the search over if this was the last searcher; other
                    // searchers otherwise look at whatever this worker moved.
                    stop_searching(true);
                    return item;
                }
            }
            {
                const std::lock_guard lock(sleep_mutex_);
                if (stopping_.load(std::memory_order_relaxed)) {
                    if (searching) {
                        idle_.fetch_sub(searching_one, std::memory_order_acq_rel);
                    }
                    if (idle_waiters_ != 0) {
                        idle_cv_.notify_all();
                    }
                    break;
                }
                sleepers_[sleeper_count_++] = &self;
                self.in_stack = true;
                self.park.store(1, std::memory_order_relaxed);
                idle_.fetch_add(sleeping_one - (searching ? searching_one : 0),
                                std::memory_order_acq_rel);
                if (idle_waiters_ != 0) {
                    idle_cv_.notify_all();
                }
            }
            if (work_item* item = scan(self)) {
                bool claimed = false;
                {
                    const std::lock_guard lock(sleep_mutex_);
                    claimed = !self.in_stack;
                    if (!claimed) {
                        remove_sleeper(self);
                    }
                }
                if (claimed) {
                    stop_searching(true); // a waker already made this worker a searcher
                } else {
                    // This worker may have been the last searcher (its registration
                    // ended the search): hand the search over, as stop_searching()
                    // does. Otherwise work the other searchers left to it, or items
                    // it just moved, could wait behind a blocked item.
                    wake_reliably();
                }
                return item;
            }
            while (self.park.load(std::memory_order_acquire) != 0) {
                self.park.wait(1, std::memory_order_acquire);
            }
            searching = true; // released by wake_one() or shutdown()
        }
        // Stopping: one last complete scan; nothing left means this worker is done.
        work_item* item = scan(self);
        if (item != nullptr) {
            wake_reliably(); // for anything this scan moved into the own ring
        }
        return item;
    }
    // Under sleep_mutex_: undo this worker's own sleep registration.
    void remove_sleeper(worker& self) noexcept {
        for (std::size_t i = 0; i < sleeper_count_; ++i) {
            if (sleepers_[i] == &self) {
                sleepers_[i] = sleepers_[--sleeper_count_];
                break;
            }
        }
        self.in_stack = false;
        self.park.store(0, std::memory_order_relaxed);
        idle_.fetch_sub(sleeping_one, std::memory_order_acq_rel);
    }
    void run_worker(worker& self) noexcept {
        current_worker = &self;
        for (;;) {
            work_item* item = find(self, false);
            if (item == nullptr) {
                item = idle(self);
                if (item == nullptr) {
                    break;
                }
            }
            // The item may destroy itself; it was unlinked before running.
            item->run(item);
            self.completed.store(self.completed.load(std::memory_order_relaxed) + 1,
                                 std::memory_order_release);
        }
        current_worker = nullptr;
    }
    // Completions are read before posts: every counted completion has its post
    // counted, so equal sums mean that every counted item has finished.
    bool idle_snapshot() const noexcept {
        std::uint64_t completed = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            completed += workers_[i].completed.load(std::memory_order_acquire);
        }
        std::uint64_t posted = injected_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count_; ++i) {
            posted += workers_[i].posted.load(std::memory_order_acquire);
        }
        return completed == posted;
    }

    inline static thread_local worker* current_worker = nullptr;

    std::size_t count_;
    std::vector<worker> workers_;   // never resized: workers have stable addresses
    std::vector<worker*> sleepers_; // stack storage; sleeper_count_ entries are live
    alignas(detail::pool_line) detail::spin_lock inject_lock_;
    work_item* inject_head_ = nullptr;
    work_item* inject_tail_ = nullptr;
    std::atomic<std::uint64_t> inject_size_{0};
    std::atomic<std::uint64_t> injected_{0};
    alignas(detail::pool_line) std::atomic<std::uint64_t> idle_{0}; // sleeping << 32 | searching
    alignas(detail::pool_line) std::mutex sleep_mutex_;
    std::condition_variable idle_cv_;
    std::size_t sleeper_count_ = 0; // guarded by sleep_mutex_
    std::size_t idle_waiters_ = 0;  // guarded by sleep_mutex_
    std::atomic<bool> stopping_{false};
    std::mutex shutdown_mutex_;
    bool joined_ = false; // guarded by shutdown_mutex_
};
} // namespace stepwright
