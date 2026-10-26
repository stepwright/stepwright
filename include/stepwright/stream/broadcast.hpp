#pragma once

#include <stepwright/detail/async_op.hpp>
#include <stepwright/detail/spin_lock.hpp>
#include <stepwright/detail/wait_core.hpp>
#include <stepwright/detail/wait_list.hpp>
#include <stepwright/stream/source.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace stepwright {
/// What a full broadcast does with a new item.
enum class overflow : std::uint8_t {
    wait,       ///< The publisher waits until the slowest subscriber frees a slot.
    drop_oldest ///< The oldest item is overwritten; slow subscribers skip it.
};

/// Single-producer, many-subscriber stream over a fixed ring of `capacity` items
/// (allocated once). Each subscription is a source<T> with its own cursor and
/// sees every item published after it subscribed, in order.
///
/// - publish/try_publish/close are called by one producer at a time; subscribe and
///   unsubscribe (subscription destruction) are thread-safe; each subscription
///   has one consumer (source<T> protocol).
/// - overflow::wait: a publish waits until the slowest subscriber frees the
///   oldest slot. overflow::drop_oldest: a publish never waits; a subscriber
///   that falls more than `capacity` items behind skips the overwritten items and
///   subscription::dropped() counts them.
/// - Each slot counts the subscribers that still have to read it; the oldest
///   slot is released when its count reaches zero, amortised O(1) per item
///   independently of the number of subscribers. With no subscriber, items are
///   discarded and publishing never waits.
/// - close(reason): subscribers drain the retained items, then end/failed(reason);
///   a waiting publish completes with errc::closed.
/// T is copied (each subscriber gets a copy) and moved under the internal lock:
/// its copy/move constructors and destructor must not throw nor call back into the
/// broadcast. No completion runs under the lock. Destroy after every subscription
/// and without a waiting publish (Debug asserts).
template <class T>
class broadcast {
    static_assert(std::is_nothrow_copy_constructible_v<T>,
                  "broadcast items are copied under a lock and must be nothrow copyable");

public:
    class publish_op;
    class subscription;

    /// Ring of capacity (>= 1; Debug asserts, 0 is treated as 1) items.
    explicit broadcast(std::size_t capacity, overflow policy = overflow::wait)
        : capacity_(std::max<std::size_t>(capacity, 1))
        , ring_(capacity_)
        , policy_(policy) {
        STEPWRIGHT_ASSERT(capacity != 0, "broadcast capacity must be at least 1");
    }
    broadcast(const broadcast&) = delete;
    broadcast& operator=(const broadcast&) = delete;
    /// Destroy only without subscriptions or a waiting publish (Debug asserts).
    ~broadcast() {
        STEPWRIGHT_ASSERT(subscribers_ == 0 && publisher_ == nullptr,
                          "destroying a broadcast with subscriptions or a waiting publish");
        for (; head_ != tail_; ++head_) {
            std::destroy_at(&at(head_).value);
        }
    }

    /// Publish without waiting; false if closed, if a publish_op is waiting, or
    /// (overflow::wait) if the ring is full. Wakes waiting subscribers.
    bool try_publish(T value) noexcept {
        detail::wait_link* readers = nullptr;
        {
            const std::lock_guard lock(lock_);
            if (closed_ || publisher_ || (policy_ == overflow::wait && full())) {
                return false;
            }
            store_locked(std::move(value));
            readers = readers_.take_all();
        }
        wake(readers);
        return true;
    }
    /// Return a publish operation for value (guaranteed copy elision).
    publish_op publish(T value) noexcept { return publish_op(*this, std::move(value)); }
    /// Close (idempotent; the first reason wins). Waiting subscribers are woken; a
    /// waiting publish completes with errc::closed.
    void close(error reason = {}) noexcept {
        detail::wait_link* readers = nullptr;
        publish_op* publisher = nullptr;
        {
            const std::lock_guard lock(lock_);
            if (closed_) {
                return;
            }
            closed_ = true;
            reason_ = std::move(reason);
            publisher = std::exchange(publisher_, nullptr);
            readers = readers_.take_all();
        }
        const detail::handoff_scope batch;
        wake(readers);
        if (publisher) {
            publisher->resolve(error::make(errc::closed));
        }
    }
    /// Thread-safe query.
    bool is_closed() const noexcept {
        const std::lock_guard lock(lock_);
        return closed_;
    }
    /// Current number of subscriptions; thread-safe snapshot.
    std::size_t subscribers() const noexcept {
        const std::lock_guard lock(lock_);
        return subscribers_;
    }
    /// Ring capacity.
    std::size_t capacity() const noexcept { return capacity_; }
    /// Subscribe; the subscription starts at the next published item (guaranteed
    /// copy elision). Equivalent to constructing subscription(*this).
    subscription subscribe() noexcept { return subscription(*this); }

private:
    struct slot {
        slot() noexcept {}
        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;
        ~slot() {}
        std::size_t remaining = 0; // subscribers that still have to read this item
        union {
            T value;
        };
    };

    slot& at(std::uint64_t sequence) noexcept {
        return ring_[static_cast<std::size_t>(sequence % capacity_)];
    }
    bool full() const noexcept { return tail_ - head_ == capacity_; }
    // Under lock_. Precondition: !full() unless the policy is drop_oldest.
    void store_locked(T&& value) noexcept {
        if (subscribers_ == 0) {
            ++tail_;
            head_ = tail_;
            return;
        }
        if (full()) {
            std::destroy_at(&at(head_).value);
            ++head_;
            release_locked();
        }
        slot& entry = at(tail_);
        std::construct_at(&entry.value, std::move(value));
        entry.remaining = subscribers_;
        ++tail_;
    }
    // Under lock_: free fully read slots at the head (amortised O(1) per item).
    void release_locked() noexcept {
        while (head_ != tail_ && at(head_).remaining == 0) {
            std::destroy_at(&at(head_).value);
            ++head_;
        }
    }
    // Under lock_, after slots were freed: admit a waiting publish.
    publish_op* admit_locked(detail::wait_link*& readers) noexcept {
        if (!publisher_ || full()) {
            return nullptr;
        }
        publish_op* granted = std::exchange(publisher_, nullptr);
        store_locked(std::move(granted->value_));
        readers = readers_.take_all();
        return granted;
    }
    // Outside lock_: complete a granted publish and the subscribers it woke.
    static void grant(publish_op* granted, detail::wait_link* readers) noexcept {
        if (!granted) {
            return;
        }
        const detail::handoff_scope batch;
        wake(readers);
        granted->resolve({});
    }
    static void wake(detail::wait_link* readers) noexcept {
        if (!readers) {
            return;
        }
        const detail::handoff_scope batch;
        while (readers) {
            detail::wait_link* next = readers->next;
            subscription::from(readers).consumer_.resolve({});
            readers = next;
        }
    }

    mutable detail::spin_lock lock_;
    std::size_t capacity_;
    std::vector<slot> ring_;
    std::uint64_t head_ = 0; // oldest retained sequence
    std::uint64_t tail_ = 0; // next sequence to publish
    std::size_t subscribers_ = 0;
    publish_op* publisher_ = nullptr;
    detail::wait_list readers_;
    error reason_;
    overflow policy_;
    bool closed_ = false;
};

/// Task publishing one value: completes once the value is in the ring (at once
/// unless overflow::wait and full), with errc::closed if the broadcast is closed
/// first, or errc::cancelled when its token is cancelled while waiting. At most
/// one publish may be outstanding (single producer; Debug asserts). Restartable;
/// must outlive its completion; the broadcast must outlive it.
template <class T>
class broadcast<T>::publish_op final : public detail::async_op {
public:
    /// Bind to owner with the value to publish.
    publish_op(broadcast& owner, T value) noexcept : owner_(&owner), value_(std::move(value)) {}
    publish_op(const publish_op&) = delete;
    publish_op& operator=(const publish_op&) = delete;
    /// Destroy only while idle (Debug asserts).
    ~publish_op() override = default;
    /// The value to publish (moved-from once published).
    T& value() noexcept { return value_; }

private:
    friend class broadcast;

    void on_start(completion done, cancel_token token) override {
        broadcast& b = *owner_;
        detail::wait_link* readers = nullptr;
        {
            std::unique_lock lock(b.lock_);
            if (b.closed_) {
                lock.unlock();
                done(error::make(errc::closed));
                return;
            }
            STEPWRIGHT_ASSERT(!b.publisher_, "concurrent broadcast publish operations");
            if (b.policy_ == overflow::wait && b.full()) {
                prepare(done, token);
                b.publisher_ = this;
                lock.unlock();
                if (token.can_be_cancelled()) {
                    arm(token, &on_cancel);
                }
                return;
            }
            b.store_locked(std::move(value_));
            readers = b.readers_.take_all();
        }
        wake(readers);
        done({});
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<publish_op*>(context);
        broadcast& b = *self.owner_;
        bool removed = false;
        {
            const std::lock_guard lock(b.lock_);
            if (b.publisher_ == &self) {
                b.publisher_ = nullptr;
                removed = true;
            }
        }
        if (removed) {
            self.resolve(error::make(errc::cancelled));
        }
    }

    broadcast* owner_;
    T value_;
};

/// A subscriber: a source<T> of every item published after it subscribed.
/// Readiness completes when an item is available or the broadcast is closed.
/// Destruction unsubscribes (releasing a waiting publish if this was the slowest
/// subscriber); destroy only without an outstanding readiness wait (Debug
/// asserts). The broadcast must outlive the subscription.
template <class T>
class broadcast<T>::subscription final : public source<T>, private detail::wait_link {
public:
    /// Subscribe to owner, starting at the next published item. Thread-safe.
    explicit subscription(broadcast& owner) noexcept : owner_(&owner) {
        const std::lock_guard lock(owner.lock_);
        cursor_ = owner.tail_;
        ++owner.subscribers_;
    }
    subscription(const subscription&) = delete;
    subscription& operator=(const subscription&) = delete;
    /// Unsubscribe. Thread-safe with the producer and other subscriptions.
    ~subscription() override {
        STEPWRIGHT_ASSERT(consumer_.idle() && !detail::wait_list::linked(*this),
                          "destroying a source with an outstanding readiness wait");
        broadcast& b = *owner_;
        detail::wait_link* readers = nullptr;
        publish_op* granted = nullptr;
        {
            const std::lock_guard lock(b.lock_);
            if (detail::wait_list::linked(*this)) {
                b.readers_.remove(*this);
            }
            --b.subscribers_;
            for (std::uint64_t seq = std::max(cursor_, b.head_); seq != b.tail_; ++seq) {
                --b.at(seq).remaining;
            }
            b.release_locked();
            granted = b.admit_locked(readers);
        }
        grant(granted, readers);
    }

    /// Items skipped because they were overwritten (overflow::drop_oldest) before
    /// this subscriber read them; consumer-side query.
    std::uint64_t dropped() const noexcept { return dropped_; }

    /// Copy the next item (releasing the slot if this was its last reader), or
    /// end/failed after close once drained, otherwise pending.
    pull<T> try_pull() override {
        STEPWRIGHT_ASSERT(consumer_.idle(), "try_pull with an outstanding readiness wait");
        broadcast& b = *owner_;
        detail::wait_link* readers = nullptr;
        publish_op* granted = nullptr;
        std::unique_lock lock(b.lock_);
        catch_up_locked();
        if (cursor_ == b.tail_) {
            if (b.closed_) {
                error reason = b.reason_;
                lock.unlock();
                return pull<T>::failure(std::move(reason));
            }
            return pull<T>::pending();
        }
        slot& entry = b.at(cursor_);
        pull<T> result = pull<T>::of(T(entry.value));
        ++cursor_;
        if (--entry.remaining == 0) {
            b.release_locked();
            granted = b.admit_locked(readers);
        }
        lock.unlock();
        grant(granted, readers);
        return result;
    }
    /// Complete when an item is available or the broadcast is closed, or with
    /// errc::cancelled.
    void on_ready(completion done, cancel_token token) override {
        STEPWRIGHT_ASSERT(consumer_.idle(), "concurrent readiness waits on a source");
        broadcast& b = *owner_;
        {
            std::unique_lock lock(b.lock_);
            catch_up_locked();
            if (cursor_ == b.tail_ && !b.closed_) {
                consumer_.prepare(done, token);
                b.readers_.push_back(*this);
                lock.unlock();
                if (token.can_be_cancelled()) {
                    consumer_.arm(token, &on_cancel, this);
                }
                return;
            }
        }
        done({});
    }

private:
    friend class broadcast;
    static subscription& from(detail::wait_link* link) noexcept {
        return static_cast<subscription&>(*link);
    }
    void catch_up_locked() noexcept {
        const broadcast& b = *owner_;
        if (cursor_ < b.head_) {
            dropped_ += b.head_ - cursor_;
            cursor_ = b.head_;
        }
    }
    static void on_cancel(void* context) noexcept {
        auto& self = *static_cast<subscription*>(context);
        broadcast& b = *self.owner_;
        bool removed = false;
        {
            const std::lock_guard lock(b.lock_);
            if (detail::wait_list::linked(self)) {
                b.readers_.remove(self);
                removed = true;
            }
        }
        if (removed) {
            self.consumer_.resolve(detail::cancelled_error());
        }
    }

    broadcast* owner_;
    std::uint64_t cursor_ = 0;
    std::uint64_t dropped_ = 0;
    detail::wait_core consumer_;
};
} // namespace stepwright
