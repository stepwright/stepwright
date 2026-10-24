#pragma once

#include <stepwright/executor.hpp>

namespace stepwright::detail {
// Per-thread hand-off trampoline used by primitives to deliver completions caused
// by a state change (set, unlock, inner completion, timer fire, ...). The queue is
// intrusive: it links the work_item embedded in the waiter, so it never allocates.
//
// * No trampoline active on this thread: run the item now, then drain everything
//   queued meanwhile, iteratively, before returning.
// * A trampoline already active: append the item and return; the outermost frame
//   runs it. Chains of hand-offs therefore use constant stack depth.
//
// Observable consequence: a completion caused by a state change may run after the
// state-changing call has returned, when that call itself was made from inside a
// completion delivered by the trampoline on the same thread.
struct handoff_queue {
    work_item* head = nullptr;
    work_item* tail = nullptr;
    bool active = false;
};

inline handoff_queue& handoff_state() noexcept {
    thread_local handoff_queue queue;
    return queue;
}

inline void handoff_drain(handoff_queue& queue) noexcept {
    while (work_item* item = queue.head) {
        queue.head = item->next;
        if (!queue.head) {
            queue.tail = nullptr;
        }
        // The item may destroy itself; it is unlinked before it runs.
        item->run(item);
    }
}

/// Deliver item through this thread's trampoline. The item must stay alive until
/// its run callback starts; the callback may destroy it.
inline void handoff(work_item& item) noexcept {
    auto& queue = handoff_state();
    if (queue.active) {
        item.next = nullptr;
        if (queue.tail) {
            queue.tail->next = &item;
        } else {
            queue.head = &item;
        }
        queue.tail = &item;
        return;
    }
    queue.active = true;
    item.run(&item);
    handoff_drain(queue);
    queue.active = false;
}

/// RAII batch: while alive, hand-offs on this thread are queued; the outermost
/// scope drains them in FIFO order when it ends. Used to deliver a batch of
/// waiters (event set, reader admission) breadth-first and in order.
class handoff_scope {
public:
    handoff_scope() noexcept : queue_(handoff_state()), owner_(!queue_.active) {
        queue_.active = true;
    }
    handoff_scope(const handoff_scope&) = delete;
    handoff_scope& operator=(const handoff_scope&) = delete;
    ~handoff_scope() {
        if (owner_) {
            handoff_drain(queue_);
            queue_.active = false;
        }
    }

private:
    handoff_queue& queue_;
    bool owner_;
};
} // namespace stepwright::detail
