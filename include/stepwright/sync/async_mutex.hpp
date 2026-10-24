#pragma once

#include <stepwright/detail/async_op.hpp>
#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/detail/wait_list.hpp>
#include <stepwright/machine.hpp>

#include <cstddef>
#include <mutex>

namespace stepwright {
/// FIFO mutual exclusion for tasks. Ownership is not tied to a thread: whoever
/// holds the lock calls unlock(), from any thread. No barging: unlock() with
/// queued waiters hands ownership directly to the oldest waiter (the mutex never
/// becomes free in between), delivering its completion through the hand-off
/// trampoline. All members are thread-safe. Must outlive every lock operation;
/// Debug asserts on destruction while locked or with waiters. No allocation.
class async_mutex {
public:
    class lock_op;
    class guarded;

    /// Construct unlocked.
    async_mutex() noexcept = default;
    async_mutex(const async_mutex&) = delete;
    async_mutex& operator=(const async_mutex&) = delete;
    /// Destroy only while unlocked without waiters (Debug asserts).
    ~async_mutex() {
        STEPWRIGHT_ASSERT(!locked_ && waiters_.empty(), "destroying a locked async_mutex");
    }

    /// Acquire without waiting; false if held or if waiters are queued.
    bool try_lock() noexcept {
        const std::lock_guard lock(lock_);
        if (locked_) {
            return false;
        }
        locked_ = true;
        return true;
    }
    /// Release (precondition: held by the caller). With waiters queued, the oldest
    /// becomes the owner and its completion runs on this thread, possibly after
    /// unlock() returns when called from inside a trampoline-delivered completion.
    void unlock() noexcept;
    /// Return a lock operation bound to this mutex (guaranteed copy elision).
    lock_op lock() noexcept;

private:
    detail::spin_lock lock_;
    bool locked_ = false;
    detail::wait_list waiters_;
};

/// Task completing successfully once the caller owns the mutex (synchronously
/// when uncontended), or with errc::cancelled, without owning it, when its token
/// is cancelled while queued. If a grant races with cancellation, exactly one
/// wins; a winning grant completes successfully and owns the lock. Restartable
/// after completion; must outlive its completion (Debug asserts).
class async_mutex::lock_op final : public detail::async_op, private detail::wait_link {
public:
    /// Bind to m without queueing.
    explicit lock_op(async_mutex& m) noexcept : mutex_(&m) {}
    lock_op(const lock_op&) = delete;
    lock_op& operator=(const lock_op&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~lock_op() override = default;

private:
    friend class async_mutex;
    static lock_op& from(detail::wait_link* link) noexcept { return static_cast<lock_op&>(*link); }

    void on_start(completion done, cancel_token token) override {
        async_mutex& m = *mutex_;
        {
            std::unique_lock lock(m.lock_);
            if (m.locked_) {
                prepare(done, token);
                m.waiters_.push_back(*this);
                lock.unlock();
                arm(token, &on_cancel);
                return;
            }
            m.locked_ = true;
        }
        done({});
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<lock_op*>(context);
        async_mutex& m = *self.mutex_;
        bool removed = false;
        {
            const std::lock_guard lock(m.lock_);
            if (detail::wait_list::linked(self)) {
                m.waiters_.remove(self);
                removed = true;
            }
        }
        if (removed) {
            self.resolve(error::make(errc::cancelled));
        }
    }

    async_mutex* mutex_;
};

inline async_mutex::lock_op async_mutex::lock() noexcept {
    return lock_op(*this);
}

inline void async_mutex::unlock() noexcept {
    detail::wait_link* next = nullptr;
    {
        const std::lock_guard lock(lock_);
        STEPWRIGHT_ASSERT(locked_, "unlock of an unlocked async_mutex");
        next = waiters_.pop_front();
        if (!next) {
            locked_ = false;
        }
    }
    if (next) {
        lock_op::from(next).resolve({});
    }
}

/// Lock m, run inner, unlock (also when inner fails), and complete with inner's
/// result. The start token reaches both the lock wait and inner; if the lock wait
/// is cancelled, inner does not run and the result is errc::cancelled. A machine:
/// restartable after completion, must outlive its completion; m and inner must
/// outlive the run.
class async_mutex::guarded final : public machine<async_mutex::guarded> {
public:
    /// Bind to m and inner (both non-owning).
    guarded(async_mutex& m, task& inner) noexcept : lock_(m), mutex_(&m), inner_(&inner) {}

private:
    friend class machine<guarded>;
    step begin() noexcept { return after<&guarded::locked>(lock_); }
    step locked() noexcept { return after<&guarded::finished>(*inner_); }
    step finished(const error& result) noexcept {
        mutex_->unlock();
        return fail(result);
    }

    lock_op lock_;
    async_mutex* mutex_;
    task* inner_;
};

class async_shared_mutex;
namespace detail {
// Common queued-waiter state of async_shared_mutex lock operations.
class rw_waiter : public async_op, protected wait_link {
protected:
    rw_waiter(async_shared_mutex& m, bool exclusive) noexcept : mutex_(&m), exclusive_(exclusive) {}
    friend class stepwright::async_shared_mutex;
    static rw_waiter& from(wait_link* link) noexcept { return static_cast<rw_waiter&>(*link); }
    void start_wait(completion done, const cancel_token& token);
    static void on_cancel(void* context) noexcept;

    async_shared_mutex* mutex_;
    bool exclusive_;
};
} // namespace detail

/// Writer-preferring reader/writer exclusion for tasks. Once a writer is queued,
/// new readers queue behind it. On release, a writer at the head gets exclusive
/// ownership; otherwise all consecutive readers at the head are admitted
/// together. Grants are delivered through the hand-off trampoline; a cancelled
/// queued waiter unregisters in O(1) and completes with errc::cancelled without
/// acquiring. All members are thread-safe. Must outlive every lock operation;
/// Debug asserts on destruction while held or with waiters. No allocation.
class async_shared_mutex {
public:
    class lock_op;
    class shared_lock_op;
    class guarded;
    class guarded_shared;

    /// Construct unlocked.
    async_shared_mutex() noexcept = default;
    async_shared_mutex(const async_shared_mutex&) = delete;
    async_shared_mutex& operator=(const async_shared_mutex&) = delete;
    /// Destroy only while unlocked without waiters (Debug asserts).
    ~async_shared_mutex() {
        STEPWRIGHT_ASSERT(!writer_ && readers_ == 0 && waiters_.empty(),
                          "destroying a held async_shared_mutex");
    }

    /// Acquire exclusively without waiting; false if held in any mode.
    bool try_lock() noexcept {
        const std::lock_guard lock(lock_);
        if (writer_ || readers_ != 0) {
            return false;
        }
        writer_ = true;
        return true;
    }
    /// Acquire shared without waiting; false if a writer holds it or any waiter
    /// is queued (writer preference).
    bool try_lock_shared() noexcept {
        const std::lock_guard lock(lock_);
        if (writer_ || !waiters_.empty()) {
            return false;
        }
        ++readers_;
        return true;
    }
    /// Release exclusive ownership (precondition: held exclusively by the caller).
    void unlock() noexcept {
        detail::wait_link* granted = nullptr;
        {
            const std::lock_guard lock(lock_);
            STEPWRIGHT_ASSERT(writer_, "unlock of an async_shared_mutex not held exclusively");
            writer_ = false;
            granted = admit();
        }
        grant(granted);
    }
    /// Release one shared ownership (precondition: held shared by the caller).
    void unlock_shared() noexcept {
        detail::wait_link* granted = nullptr;
        {
            const std::lock_guard lock(lock_);
            STEPWRIGHT_ASSERT(readers_ != 0,
                              "unlock_shared of an async_shared_mutex not held shared");
            if (--readers_ == 0) {
                granted = admit();
            }
        }
        grant(granted);
    }
    /// Return an exclusive lock operation (guaranteed copy elision).
    lock_op lock() noexcept;
    /// Return a shared lock operation (guaranteed copy elision).
    shared_lock_op lock_shared() noexcept;

private:
    friend class detail::rw_waiter;

    // Under lock_: move every waiter that can now acquire into a private chain
    // (linked through next, FIFO) and update the ownership counters.
    detail::wait_link* admit() noexcept {
        detail::wait_link* granted = nullptr;
        detail::wait_link** tail = &granted;
        while (!writer_) {
            detail::wait_link* head = waiters_.front();
            if (!head) {
                break;
            }
            const bool exclusive = detail::rw_waiter::from(head).exclusive_;
            if (exclusive) {
                if (readers_ != 0) {
                    break;
                }
                writer_ = true;
            } else {
                ++readers_;
            }
            waiters_.remove(*head);
            *tail = head;
            tail = &head->next;
        }
        return granted;
    }
    // Outside lock_: complete the granted waiters in order.
    static void grant(detail::wait_link* granted) noexcept {
        if (!granted) {
            return;
        }
        const detail::handoff_scope batch;
        while (granted) {
            detail::wait_link* next = granted->next;
            detail::rw_waiter::from(granted).resolve({});
            granted = next;
        }
    }

    detail::spin_lock lock_;
    bool writer_ = false;
    std::size_t readers_ = 0;
    detail::wait_list waiters_;
};

/// Task completing once the caller holds the mutex exclusively (synchronously
/// when free), or with errc::cancelled when cancelled while queued. Same grant/
/// cancel, restart and lifetime rules as async_mutex::lock_op.
class async_shared_mutex::lock_op final : public detail::rw_waiter {
public:
    /// Bind to m without queueing.
    explicit lock_op(async_shared_mutex& m) noexcept : rw_waiter(m, true) {}

private:
    void on_start(completion done, cancel_token token) override { start_wait(done, token); }
};

/// Task completing once the caller holds the mutex shared (synchronously when no
/// writer holds it and none is queued), or with errc::cancelled when cancelled
/// while queued. Same rules as lock_op.
class async_shared_mutex::shared_lock_op final : public detail::rw_waiter {
public:
    /// Bind to m without queueing.
    explicit shared_lock_op(async_shared_mutex& m) noexcept : rw_waiter(m, false) {}

private:
    void on_start(completion done, cancel_token token) override { start_wait(done, token); }
};

inline async_shared_mutex::lock_op async_shared_mutex::lock() noexcept {
    return lock_op(*this);
}
inline async_shared_mutex::shared_lock_op async_shared_mutex::lock_shared() noexcept {
    return shared_lock_op(*this);
}

inline void detail::rw_waiter::start_wait(completion done, const cancel_token& token) {
    async_shared_mutex& m = *mutex_;
    {
        std::unique_lock lock(m.lock_);
        const bool available =
            exclusive_ ? !m.writer_ && m.readers_ == 0 : !m.writer_ && m.waiters_.empty();
        if (!available) {
            prepare(done, token);
            m.waiters_.push_back(*this);
            lock.unlock();
            arm(token, &on_cancel);
            return;
        }
        STEPWRIGHT_ASSERT(m.waiters_.empty(), "async_shared_mutex free with queued waiters");
        if (exclusive_) {
            m.writer_ = true;
        } else {
            ++m.readers_;
        }
    }
    done({});
}

inline void detail::rw_waiter::on_cancel(void* context) noexcept {
    auto& self = *static_cast<rw_waiter*>(context);
    async_shared_mutex& m = *self.mutex_;
    bool removed = false;
    wait_link* granted = nullptr;
    {
        const std::lock_guard lock(m.lock_);
        if (wait_list::linked(self)) {
            m.waiters_.remove(self);
            removed = true;
            // A cancelled writer may have been holding back readers.
            granted = m.admit();
        }
    }
    if (removed) {
        const handoff_scope batch;
        self.resolve(error::make(errc::cancelled));
        async_shared_mutex::grant(granted);
    }
}

/// Exclusive variant of async_mutex::guarded for async_shared_mutex.
class async_shared_mutex::guarded final : public machine<async_shared_mutex::guarded> {
public:
    /// Bind to m and inner (both non-owning).
    guarded(async_shared_mutex& m, task& inner) noexcept : lock_(m), mutex_(&m), inner_(&inner) {}

private:
    friend class machine<guarded>;
    step begin() noexcept { return after<&guarded::locked>(lock_); }
    step locked() noexcept { return after<&guarded::finished>(*inner_); }
    step finished(const error& result) noexcept {
        mutex_->unlock();
        return fail(result);
    }

    lock_op lock_;
    async_shared_mutex* mutex_;
    task* inner_;
};

/// Shared variant: lock_shared, run inner, unlock_shared, complete with inner's
/// result. Same token, restart and lifetime rules as async_mutex::guarded.
class async_shared_mutex::guarded_shared final
    : public machine<async_shared_mutex::guarded_shared> {
public:
    /// Bind to m and inner (both non-owning).
    guarded_shared(async_shared_mutex& m, task& inner) noexcept
        : lock_(m)
        , mutex_(&m)
        , inner_(&inner) {}

private:
    friend class machine<guarded_shared>;
    step begin() noexcept { return after<&guarded_shared::locked>(lock_); }
    step locked() noexcept { return after<&guarded_shared::finished>(*inner_); }
    step finished(const error& result) noexcept {
        mutex_->unlock_shared();
        return fail(result);
    }

    shared_lock_op lock_;
    async_shared_mutex* mutex_;
    task* inner_;
};
} // namespace stepwright
