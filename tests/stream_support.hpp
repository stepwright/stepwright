#pragma once

#include "test_support.hpp"
#include <atomic>
#include <vector>

namespace stream_support {
// Source wrapping an unbounded channel that counts readiness waits and how many of
// them completed with errc::cancelled (merge tests).
template <class T>
struct probe_source final : stepwright::source<T> {
    stepwright::channel<T> inner;
    std::atomic<int> waits{0};
    std::atomic<int> cancelled{0};
    stepwright::completion receiver;

    stepwright::pull<T> try_pull() override { return inner.try_pull(); }
    void on_ready(stepwright::completion done, stepwright::cancel_token token) override {
        waits.fetch_add(1, std::memory_order_relaxed);
        receiver = done;
        inner.on_ready({.fn = &on_inner, .ctx = this}, std::move(token));
    }
    static void on_inner(void* ctx, const stepwright::error& e) noexcept {
        auto& self = *static_cast<probe_source*>(ctx);
        if (e.is(stepwright::errc::cancelled)) {
            self.cancelled.fetch_add(1, std::memory_order_relaxed);
        }
        const auto done = self.receiver;
        done(e);
    }
};

// Pull every item of an always-ready source until it ends or fails.
template <class T>
std::vector<T> drain(stepwright::source<T>& s, stepwright::error* failure = nullptr) {
    std::vector<T> items;
    for (;;) {
        auto next = s.try_pull();
        if (next.has_item()) {
            items.push_back(std::move(next).value());
            continue;
        }
        if (next.is_failed() && failure) {
            *failure = next.failure_reason();
        }
        return items;
    }
}

// Pull one item, waiting for readiness with a blocking receiver (any thread).
template <class T>
stepwright::pull<T> pull_blocking(stepwright::source<T>& s) {
    for (;;) {
        auto next = s.try_pull();
        if (!next.is_pending()) {
            return next;
        }
        test_support::receiver ready;
        s.on_ready(ready.callback(), {});
        ready.wait();
        if (ready.result) {
            return stepwright::pull<T>::failure(ready.result);
        }
    }
}
} // namespace stream_support
