#pragma once

#include <stepwright/stepwright.hpp>

#include "stress.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace test_support {
struct receiver {
    stepwright::error result;
    int calls = 0;
    std::atomic<bool> completed{false};
    void finish(const stepwright::error& e) noexcept {
        result = e;
        ++calls;
        completed.store(true, std::memory_order_release);
    }
    stepwright::completion callback() noexcept {
        return stepwright::completion::bind<&receiver::finish>(this);
    }
    void wait() const noexcept {
        while (!completed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};
struct sync_child : stepwright::task {
    stepwright::error result;
    int calls = 0;
    void on_start(stepwright::completion done, stepwright::cancel_token) override {
        ++calls;
        auto copy = result;
        done(copy);
    }
};
struct async_child : stepwright::task {
    stepwright::completion receiver;
    stepwright::cancel_token received_token;
    void on_start(stepwright::completion done, stepwright::cancel_token token) override {
        receiver = done;
        received_token = std::move(token);
    }
    void finish(const stepwright::error& result = {}) {
        const auto done = receiver;
        done(result);
    }
};
struct parked : stepwright::machine<parked> {
    step begin() { return park<&parked::finish>(); }
    step finish(const stepwright::error& e) { return fail(e); }
};
// Asynchronous child that completes with errc::cancelled when its token is
// cancelled, or with an explicit result via complete(); exactly one wins.
struct cancellable_child : stepwright::task {
    stepwright::completion receiver;
    stepwright::cancel_token received_token;
    stepwright::cancel_registration registration;
    std::atomic<bool> pending{false};
    std::atomic<int> starts{0};
    std::atomic<int> cancelled{0};
    void on_start(stepwright::completion done, stepwright::cancel_token token) override {
        receiver = done;
        received_token = token;
        starts.fetch_add(1, std::memory_order_relaxed);
        pending.store(true, std::memory_order_release);
        registration.arm(
            token,
            [](void* ctx) noexcept {
                static_cast<cancellable_child*>(ctx)->complete(
                    stepwright::error::make(stepwright::errc::cancelled));
            },
            this);
    }
    // Returns whether this call delivered the completion.
    bool complete(const stepwright::error& result = {}) {
        if (!pending.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        registration.disarm();
        if (result.is(stepwright::errc::cancelled)) {
            cancelled.fetch_add(1, std::memory_order_relaxed);
        }
        const auto done = receiver;
        done(result);
        return true;
    }
};
// Tracks the span of frame addresses seen; robust under ASan's fake stacks.
struct depth_probe {
    std::uintptr_t low = std::numeric_limits<std::uintptr_t>::max();
    std::uintptr_t high = 0;
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void
    sample() noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
        low = std::min(low, address);
        high = std::max(high, address);
    }
    std::uintptr_t span() const noexcept { return high - low; }
};
// Runs each action on its own persistent thread once per round, released
// together. Hand-offs use only atomics (TSan-visible on every platform; libc++'s
// std::barrier lives in an uninstrumented library on macOS). Data written before
// run_round() is visible to the actions; their effects are visible after it.
class race_rounds {
public:
    explicit race_rounds(std::vector<std::function<void()>> actions)
        : actions_(std::move(actions))
        , finished_(actions_.size()) {
        for (std::size_t i = 0; i < actions_.size(); ++i) {
            threads_.emplace_back([this, i] { work(i); });
        }
    }
    race_rounds(const race_rounds&) = delete;
    race_rounds& operator=(const race_rounds&) = delete;
    ~race_rounds() {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            thread.join();
        }
    }
    // Release one round; optionally run main_action concurrently on this thread;
    // return once every action has finished.
    template <class F>
    void run_round(F&& main_action) {
        const int round = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::forward<F>(main_action)();
        for (auto& finished : finished_) {
            while (finished.load(std::memory_order_acquire) != round) {
                std::this_thread::yield();
            }
        }
    }
    void run_round() {
        run_round([] {});
    }

private:
    void work(std::size_t index) {
        int seen = 0;
        for (;;) {
            int now = epoch_.load(std::memory_order_acquire);
            while (now == seen) {
                if (stop_.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
                now = epoch_.load(std::memory_order_acquire);
            }
            seen = now;
            actions_[index]();
            finished_[index].store(seen, std::memory_order_release);
        }
    }
    std::vector<std::function<void()>> actions_;
    std::vector<std::atomic<int>> finished_;
    std::atomic<int> epoch_{0};
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};
// Busy-wait a pseudo-random number of microseconds below limit (timing jitter).
inline void jitter(unsigned& seed, unsigned limit_us) {
    seed = seed * 1664525U + 1013904223U;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::microseconds((seed >> 16) % limit_us);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
}
} // namespace test_support
