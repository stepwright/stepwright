#pragma once

#include <stepwright/stepwright.hpp>

#include "stress.hpp"
#include <atomic>
#include <thread>
#include <utility>

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
} // namespace test_support
