#pragma once

namespace stepwright::detail {
// Intrusive FIFO node embedded in a waiter. prev points at the previous node's
// next field (or the list head); a null prev means "not linked". All fields are
// protected by the owning primitive's lock while linked.
struct wait_link {
    wait_link* next = nullptr;
    wait_link** prev = nullptr;
};

// Doubly linked intrusive FIFO with O(1) push, pop and removal from any position.
// Not thread-safe: the owning primitive serializes access with its lock.
class wait_list {
public:
    wait_list() noexcept = default;
    wait_list(const wait_list&) = delete;
    wait_list& operator=(const wait_list&) = delete;
    ~wait_list() = default;

    bool empty() const noexcept { return head_ == nullptr; }
    wait_link* front() const noexcept { return head_; }
    static bool linked(const wait_link& node) noexcept { return node.prev != nullptr; }

    void push_back(wait_link& node) noexcept {
        node.next = nullptr;
        node.prev = tail_;
        *tail_ = &node;
        tail_ = &node.next;
    }
    void remove(wait_link& node) noexcept {
        *node.prev = node.next;
        if (node.next) {
            node.next->prev = node.prev;
        } else {
            tail_ = node.prev;
        }
        node.next = nullptr;
        node.prev = nullptr;
    }
    wait_link* pop_front() noexcept {
        wait_link* node = head_;
        if (node) {
            remove(*node);
        }
        return node;
    }
    /// Detach every node, marking each unlinked, and return the chain (via next)
    /// in FIFO order. O(n) under the caller's lock; the chain is then private.
    wait_link* take_all() noexcept {
        wait_link* chain = head_;
        for (wait_link* node = chain; node; node = node->next) {
            node->prev = nullptr;
        }
        head_ = nullptr;
        tail_ = &head_;
        return chain;
    }

private:
    wait_link* head_ = nullptr;
    wait_link** tail_ = &head_;
};
} // namespace stepwright::detail
