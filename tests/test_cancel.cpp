#include <stepwright/cancel.hpp>

#include <gtest/gtest.h>

#include "stress.hpp"
#include <algorithm>
#include <atomic>
#include <latch>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {
template <class S>
concept publicly_resettable = requires(S& source) { source.reset(); };
// Rearming is internal to the library's combinators (spec M3 §0.1).
void reset(stepwright::cancel_source& source) noexcept {
    stepwright::detail::cancel_source_access::reset(source);
}
} // namespace

TEST(cancel, regression_cancellation_is_one_way_publicly) {
    static_assert(!publicly_resettable<stepwright::cancel_source>);
    stepwright::cancel_source source;
    EXPECT_TRUE(source.request_cancel());
    EXPECT_FALSE(source.request_cancel());
    EXPECT_TRUE(source.cancel_requested());
}
TEST(cancel, before_after_and_deregister) {
    stepwright::cancel_source source;
    EXPECT_TRUE(source.token().can_be_cancelled());
    EXPECT_FALSE(source.cancel_requested());
    int calls = 0;
    {
        stepwright::cancel_callback removed(source.token(), [&] { ++calls; });
    }
    stepwright::cancel_callback active(source.token(), [&] { ++calls; });
    EXPECT_TRUE(source.request_cancel());
    EXPECT_FALSE(source.request_cancel());
    EXPECT_TRUE(source.cancel_requested());
    EXPECT_EQ(calls, 1);
    stepwright::cancel_callback immediate(source.token(), [&] { ++calls; });
    EXPECT_EQ(calls, 2);
    stepwright::cancel_token empty;
    EXPECT_FALSE(empty.can_be_cancelled());
    EXPECT_FALSE(empty.cancel_requested());
    stepwright::cancel_callback ignored(empty, [&] { ++calls; });
    EXPECT_EQ(calls, 2);
}
TEST(cancel, regression_self_deregistration) {
    stepwright::cancel_source source;
    struct self_remove {
        std::unique_ptr<stepwright::cancel_callback<self_remove>>* owner;
        int* calls;
        void operator()() const noexcept {
            ++*calls;
            owner->reset();
        }
    };
    int calls = 0;
    std::unique_ptr<stepwright::cancel_callback<self_remove>> callback;
    callback = std::make_unique<stepwright::cancel_callback<self_remove>>(
        source.token(), self_remove{.owner = &callback, .calls = &calls});
    EXPECT_TRUE(source.request_cancel());
    EXPECT_FALSE(callback);
    EXPECT_EQ(calls, 1);
}
TEST(cancel, regression_foreign_destructor_waits) {
    stepwright::cancel_source source;
    std::latch entered(1);
    std::latch release(1);
    std::latch destroying(1);
    std::atomic<bool> destroyed{false};
    auto fn = [&] {
        entered.count_down();
        release.wait();
    };
    auto callback = std::make_unique<stepwright::cancel_callback<decltype(fn)>>(source.token(), fn);
    std::thread requester([&] { source.request_cancel(); });
    entered.wait();
    std::thread destroyer([&] {
        destroying.count_down();
        callback.reset();
        destroyed.store(true, std::memory_order_release);
    });
    destroying.wait();
    EXPECT_FALSE(destroyed.load(std::memory_order_acquire));
    release.count_down();
    requester.join();
    destroyer.join();
    EXPECT_TRUE(destroyed.load());
}
TEST(cancel, regression_request_destroy_stress) {
    for (int i = 0; i < test_support::stress_iterations(1000); ++i) {
        stepwright::cancel_source source;
        std::atomic<int> calls{0};
        auto fn = [&] { calls.fetch_add(1, std::memory_order_relaxed); };
        auto callback =
            std::make_unique<stepwright::cancel_callback<decltype(fn)>>(source.token(), fn);
        std::thread requester([&] { source.request_cancel(); });
        if ((i % 2) == 0) {
            std::this_thread::yield();
        }
        callback.reset();
        requester.join();
        EXPECT_LE(calls.load(), 1);
    }
}
TEST(cancel, regression_deregister_head_middle_tail_random) {
    const auto size = static_cast<std::size_t>(test_support::stress_iterations(1000));
    for (int order = 0; order < 4; ++order) {
        stepwright::cancel_source source;
        std::vector<stepwright::cancel_registration> nodes(size);
        std::vector<int> calls(size, 0);
        const auto callback = [](void* ctx) noexcept { ++*static_cast<int*>(ctx); };
        for (std::size_t i = 0; i < size; ++i) {
            nodes[i].arm(source.token(), callback, &calls[i]);
        }
        std::vector<std::size_t> removal(size);
        std::iota(removal.begin(), removal.end(), 0U);
        if (order == 0) {
            std::ranges::reverse(removal);
        }
        if (order == 1) {
            std::ranges::rotate(removal, removal.begin() + static_cast<std::ptrdiff_t>(size / 2));
        }
        if (order == 3) {
            std::mt19937 random(42);
            std::shuffle(removal.begin(), removal.end(), random);
        }
        // Leave half registered, so request_cancel also exercises unlinking the
        // remaining list and never invokes a removed node.
        for (std::size_t i = 0; i < size / 2; ++i) {
            nodes[removal[i]].disarm();
        }
        source.request_cancel();
        for (std::size_t i = 0; i < size; ++i) {
            EXPECT_EQ(calls[removal[i]], i < size / 2 ? 0 : 1);
        }
    }
}
TEST(cancel, regression_self_rearm_preserves_new_linkage) {
    stepwright::cancel_source first;
    stepwright::cancel_source second;
    stepwright::cancel_registration node;
    struct context {
        stepwright::cancel_registration* node;
        stepwright::cancel_token next;
        int calls = 0;
    } ctx{.node = &node, .next = second.token()};
    node.arm(
        first.token(),
        [](void* ptr) noexcept {
            auto& state = *static_cast<context*>(ptr);
            ++state.calls;
            state.node->disarm();
            state.node->arm(
                state.next, [](void* count) noexcept { ++*static_cast<int*>(count); },
                &state.calls);
        },
        &ctx);
    first.request_cancel();
    EXPECT_EQ(ctx.calls, 1);
    second.request_cancel();
    EXPECT_EQ(ctx.calls, 2);
}
TEST(cancel, registration_rearm_and_token_lifetime) {
    stepwright::cancel_registration node;
    int calls = 0;
    const auto fn = [](void* ctx) noexcept { ++*static_cast<int*>(ctx); };
    stepwright::cancel_token token;
    {
        stepwright::cancel_source source;
        token = source.token();
        node.arm(token, fn, &calls);
        node.arm({}, fn, &calls);
        source.request_cancel();
        EXPECT_EQ(calls, 0);
    }
    node.arm(token, fn, &calls);
    EXPECT_EQ(calls, 1);
}
TEST(cancel, regression_moved_source_is_inert) {
    stepwright::cancel_source source;
    auto moved = std::move(source);
    // NOLINTNEXTLINE(bugprone-use-after-move): regression for the inert moved-from source.
    EXPECT_FALSE(source.request_cancel());
    EXPECT_FALSE(source.cancel_requested());
    EXPECT_FALSE(source.token().can_be_cancelled());
    EXPECT_TRUE(moved.request_cancel());
}
TEST(cancel, regression_immediate_callback_destroys_registration_and_source) {
    auto source = std::make_unique<stepwright::cancel_source>();
    source->request_cancel();
    auto registration = std::make_unique<stepwright::cancel_registration>();
    struct context {
        std::unique_ptr<stepwright::cancel_source>* source;
        std::unique_ptr<stepwright::cancel_registration>* registration;
    } ctx{.source = &source, .registration = &registration};
    registration->arm(
        source->token(),
        [](void* ptr) noexcept {
            auto& values = *static_cast<context*>(ptr);
            values.source->reset();
            values.registration->reset();
        },
        &ctx);
    EXPECT_FALSE(source);
    EXPECT_FALSE(registration);
}
TEST(cancel, reset_allows_reuse_without_allocation) {
    stepwright::cancel_source source;
    auto stale = source.token();
    int calls = 0;
    {
        stepwright::cancel_callback callback(source.token(), [&] { ++calls; });
        EXPECT_TRUE(source.request_cancel());
    }
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(stale.cancel_requested());
    reset(source);
    EXPECT_FALSE(source.cancel_requested());
    EXPECT_FALSE(stale.cancel_requested()); // tokens share the reset state
    stepwright::cancel_callback again(source.token(), [&] { ++calls; });
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(source.request_cancel());
    EXPECT_EQ(calls, 2);
    stepwright::cancel_source moved = std::move(source);
    // NOLINTNEXTLINE(bugprone-use-after-move): reset of a moved-from source is a no-op.
    reset(source);
    EXPECT_TRUE(moved.cancel_requested());
}
TEST(cancel, regression_reset_with_armed_registration_asserts) {
#if STEPWRIGHT_DEBUG
    EXPECT_DEATH(
        {
            stepwright::cancel_source source;
            stepwright::cancel_callback callback(source.token(), [] {});
            reset(source);
        },
        "cancel_source reset with armed registrations");
#else
    GTEST_SKIP() << "STEPWRIGHT_DEBUG is disabled";
#endif
}
