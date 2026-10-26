#pragma once

#include <stepwright/detail/async_op.hpp>
#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/detail/wait_core.hpp>
#include <stepwright/detail/wait_list.hpp>
#include <stepwright/stream/source.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

namespace stepwright {
namespace detail {
// Ring buffer of T over raw storage: items are constructed on push and destroyed
// on pop. Not synchronized; the owner locks around every call.
template <class T>
class ring {
public:
    ring() noexcept = default;
    explicit ring(std::size_t capacity) : data_(allocate(capacity)), capacity_(capacity) {}
    ring(const ring&) = delete;
    ring& operator=(const ring&) = delete;
    ~ring() {
        while (count_ != 0) {
            pop_discard();
        }
        deallocate(data_, capacity_);
    }

    std::size_t size() const noexcept { return count_; }
    std::size_t capacity() const noexcept { return capacity_; }
    bool full() const noexcept { return count_ == capacity_; }
    /// Precondition: !full().
    void push(T&& value) noexcept {
        std::size_t index = head_ + count_;
        if (index >= capacity_) {
            index -= capacity_;
        }
        std::construct_at(data_ + index, std::move(value));
        ++count_;
    }
    /// Precondition: size() != 0.
    T pop() noexcept {
        T value = std::move(data_[head_]);
        pop_discard();
        return value;
    }
    /// Move the items into fresh storage of the given capacity (> size()) and
    /// return the old storage, which the caller frees with deallocate().
    std::pair<T*, std::size_t> adopt(T* fresh, std::size_t capacity) noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            std::size_t index = head_ + i;
            if (index >= capacity_) {
                index -= capacity_;
            }
            std::construct_at(fresh + i, std::move(data_[index]));
            std::destroy_at(data_ + index);
        }
        const std::pair<T*, std::size_t> old{data_, capacity_};
        data_ = fresh;
        capacity_ = capacity;
        head_ = 0;
        return old;
    }
    static T* allocate(std::size_t capacity) {
        return capacity == 0 ? nullptr : std::allocator<T>().allocate(capacity);
    }
    static void deallocate(T* data, std::size_t capacity) noexcept {
        if (data) {
            std::allocator<T>().deallocate(data, capacity);
        }
    }

private:
    void pop_discard() noexcept {
        std::destroy_at(data_ + head_);
        head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
        --count_;
    }

    T* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};
} // namespace detail

/// Multi-producer / single-consumer queue that is a source<T>. Bounded channels
/// allocate their ring buffer once at construction; unbounded channels grow it
/// (doubling, amortised) and are allocation-free once large enough.
///
/// - Producers (try_send, send, close) are thread-safe. The consumer side follows
///   the source<T> protocol (one consumer at a time).
/// - Waiting senders (bounded, full) queue FIFO and are granted space in order;
///   a sender is never overtaken by try_send while it waits.
/// - close(reason) lets the consumer drain buffered items, then try_pull returns
///   end (empty reason) or failed(reason). Waiting senders complete with
///   errc::closed and their values stay in their send_op.
/// - Checking for an item and registering the consumer's readiness wait happen in
///   one critical section, and exactly one producer (or close, or the wait's
///   cancellation) claims a registered wait.
/// T's move constructor and destructor run under the channel's internal lock: they
/// must not throw and must not call back into the channel. No completion or other
/// user code runs under the lock. Destroy only without waiting senders and
/// without an outstanding readiness wait (Debug asserts).
template <class T>
class channel final : public source<T> {
public:
    class send_op;

    /// Bounded channel holding at most capacity items (capacity >= 1; Debug
    /// asserts, 0 is treated as 1). Allocates the buffer once.
    explicit channel(std::size_t capacity)
        : bounded_(true)
        , ring_(std::max<std::size_t>(capacity, 1)) {
        STEPWRIGHT_ASSERT(capacity != 0, "channel capacity must be at least 1");
    }
    /// Unbounded channel; allocates when the buffer has to grow.
    channel() noexcept = default;
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    /// Destroy only without waiting senders or an outstanding readiness wait.
    ~channel() override {
        STEPWRIGHT_ASSERT(!waiting_ && consumer_.idle(),
                          "destroying a source with an outstanding readiness wait");
        STEPWRIGHT_ASSERT(senders_.empty(), "destroying a channel with waiting senders");
    }

    /// Enqueue value without waiting. false if the channel is closed, or bounded
    /// and full (value is then dropped). Unbounded channels may allocate to grow
    /// (and may then throw std::bad_alloc). Thread-safe.
    bool try_send(T value) {
        T* retired = nullptr;
        std::size_t retired_capacity = 0;
        bool wake = false;
        {
            const std::lock_guard lock(lock_);
            if (closed_) {
                return false;
            }
            if (ring_.full()) {
                if (bounded_) {
                    return false;
                }
                std::tie(retired, retired_capacity) = grow_locked();
            }
            ring_.push(std::move(value));
            wake = std::exchange(waiting_, false);
        }
        detail::ring<T>::deallocate(retired, retired_capacity);
        if (wake) {
            consumer_.resolve({});
        }
        return true;
    }
    /// Return a send operation for value (guaranteed copy elision); see send_op.
    send_op send(T value) noexcept { return send_op(*this, std::move(value)); }
    /// Close the channel (idempotent: the first reason wins). Buffered items stay
    /// available; waiting senders complete with errc::closed; a waiting consumer is
    /// woken. Completions run on this thread through the hand-off trampoline.
    void close(error reason = {}) noexcept;
    /// Thread-safe snapshot, not a destruction signal (close may still notify).
    bool is_closed() const noexcept {
        const std::lock_guard lock(lock_);
        return closed_;
    }
    /// Number of buffered items (excluding waiting senders); thread-safe snapshot,
    /// not a destruction signal (send/receive may still deliver completions).
    std::size_t size() const noexcept {
        const std::lock_guard lock(lock_);
        return ring_.size();
    }
    /// The bound, or 0 for an unbounded channel; not a destruction signal.
    std::size_t capacity() const noexcept { return bounded_ ? ring_.capacity() : 0; }

    /// Consumer: take the oldest item (admitting the oldest waiting sender into the
    /// freed slot), or end/failed after close once drained, otherwise pending.
    pull<T> try_pull() override {
        send_op* granted = nullptr;
        std::unique_lock lock(lock_);
        STEPWRIGHT_ASSERT(!waiting_, "try_pull with an outstanding readiness wait");
        if (ring_.size() != 0) {
            pull<T> result = pull<T>::of(ring_.pop());
            if (detail::wait_link* link = senders_.pop_front()) {
                granted = &send_op::from(link);
                ring_.push(std::move(granted->value_));
            }
            lock.unlock();
            if (granted) {
                granted->resolve({});
            }
            return result;
        }
        if (closed_) {
            error reason = reason_;
            lock.unlock();
            return pull<T>::failure(std::move(reason));
        }
        return pull<T>::pending();
    }
    /// Consumer: complete now if an item is buffered or the channel is closed;
    /// otherwise when a producer sends or closes, or with errc::cancelled.
    void on_ready(completion done, cancel_token token) override {
        STEPWRIGHT_ASSERT(consumer_.idle(), "concurrent readiness waits on a source");
        {
            std::unique_lock lock(lock_);
            if (ring_.size() == 0 && !closed_) {
                consumer_.prepare(done, token);
                waiting_ = true;
                lock.unlock();
                if (token.can_be_cancelled()) {
                    consumer_.arm(token, &on_cancel_wait, this);
                }
                return;
            }
        }
        done({});
    }

private:
    std::pair<T*, std::size_t> grow_locked() {
        const std::size_t capacity = std::max<std::size_t>(ring_.capacity() * 2, 8);
        return ring_.adopt(detail::ring<T>::allocate(capacity), capacity);
    }
    static void on_cancel_wait(void* context) noexcept {
        auto& self = *static_cast<channel*>(context);
        {
            const std::lock_guard lock(self.lock_);
            if (!self.waiting_) {
                return;
            }
            self.waiting_ = false;
        }
        self.consumer_.resolve(detail::cancelled_error());
    }

    mutable detail::spin_lock lock_;
    bool bounded_ = false;
    bool closed_ = false;
    bool waiting_ = false; // consumer readiness wait registered
    detail::ring<T> ring_;
    error reason_;
    detail::wait_list senders_;
    detail::wait_core consumer_;
};

/// Task sending one value: completes successfully once the value is enqueued
/// (synchronously when there is room), waits FIFO while a bounded channel is full,
/// completes with errc::closed if the channel is closed before the value is
/// enqueued, or with errc::cancelled when its token is cancelled while waiting (it
/// unregisters in O(1)). After closed/cancelled the value stays in value().
/// Restartable; must outlive its completion (Debug asserts); the channel must
/// outlive the operation.
template <class T>
class channel<T>::send_op final : public detail::async_op, private detail::wait_link {
public:
    /// Bind to ch with the value to send.
    send_op(channel& ch, T value) noexcept : channel_(&ch), value_(std::move(value)) {}
    send_op(const send_op&) = delete;
    send_op& operator=(const send_op&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~send_op() override = default;
    /// The value to send (moved-from once it has been enqueued).
    T& value() noexcept { return value_; }

private:
    friend class channel;
    static send_op& from(detail::wait_link* link) noexcept { return static_cast<send_op&>(*link); }

    void on_start(completion done, cancel_token token) override {
        channel& ch = *channel_;
        T* retired = nullptr;
        std::size_t retired_capacity = 0;
        bool wake = false;
        {
            std::unique_lock lock(ch.lock_);
            if (ch.closed_) {
                lock.unlock();
                done(error::make(errc::closed));
                return;
            }
            if (ch.ring_.full()) {
                if (ch.bounded_) {
                    // Senders queue only while the buffer is full, so FIFO holds.
                    prepare(done, token);
                    ch.senders_.push_back(*this);
                    lock.unlock();
                    if (token.can_be_cancelled()) {
                        arm(token, &on_cancel);
                    }
                    return;
                }
                std::tie(retired, retired_capacity) = ch.grow_locked();
            }
            ch.ring_.push(std::move(value_));
            wake = std::exchange(ch.waiting_, false);
        }
        detail::ring<T>::deallocate(retired, retired_capacity);
        if (wake) {
            ch.consumer_.resolve({});
        }
        done({});
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<send_op*>(context);
        channel& ch = *self.channel_;
        bool removed = false;
        {
            const std::lock_guard lock(ch.lock_);
            if (detail::wait_list::linked(self)) {
                ch.senders_.remove(self);
                removed = true;
            }
        }
        if (removed) {
            self.resolve(error::make(errc::cancelled));
        }
    }

    channel* channel_;
    T value_;
};

template <class T>
void channel<T>::close(error reason) noexcept {
    detail::wait_link* chain = nullptr;
    bool wake = false;
    {
        const std::lock_guard lock(lock_);
        if (closed_) {
            return;
        }
        closed_ = true;
        reason_ = std::move(reason);
        chain = senders_.take_all();
        wake = std::exchange(waiting_, false);
    }
    // Detached senders are private to this call; the channel is not touched after
    // the consumer's wait is resolved (its receiver may destroy the channel).
    const detail::handoff_scope batch;
    if (chain) {
        const error closed = error::make(errc::closed);
        while (chain) {
            detail::wait_link* next = chain->next;
            send_op::from(chain).resolve(closed);
            chain = next;
        }
    }
    if (wake) {
        consumer_.resolve({});
    }
}
} // namespace stepwright
