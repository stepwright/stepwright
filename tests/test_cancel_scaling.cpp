// Striped cancellation state under contention, and lazy machine registration
// (docs/design/performance.md).
#include <stepwright/stepwright.hpp>

#include <gtest/gtest.h>

#include "test_support.hpp"
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t thread_count = 8;
constexpr std::size_t nodes_per_thread = 64;

struct counted_node {
    stepwright::cancel_registration registration;
    std::atomic<int> calls{0};
    static void call(void* ctx) noexcept {
        static_cast<counted_node*>(ctx)->calls.fetch_add(1, std::memory_order_relaxed);
    }
};

std::vector<std::function<void()>> per_thread(const std::function<void(std::size_t)>& action) {
    std::vector<std::function<void()>> actions;
    actions.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
        actions.emplace_back([action, t] { action(t); });
    }
    return actions;
}
} // namespace

// Eight threads arm and disarm their own registrations on one shared token at the
// same time; a request afterwards invokes exactly the ones left armed, once.
TEST(cancel_scaling, regression_contended_arm_disarm_then_request) {
    std::vector<counted_node> nodes(thread_count * nodes_per_thread);
    std::vector<char> kept(nodes.size(), 0);
    stepwright::cancel_source* source = nullptr;
    test_support::race_rounds rounds(per_thread([&](std::size_t t) {
        std::minstd_rand random(static_cast<unsigned>(t + 1));
        const auto token = source->token();
        for (int pass = 0; pass < 8; ++pass) {
            for (std::size_t i = 0; i < nodes_per_thread; ++i) {
                nodes[(t * nodes_per_thread) + i].registration.arm(
                    token, &counted_node::call, &nodes[(t * nodes_per_thread) + i]);
            }
            for (std::size_t i = 0; i < nodes_per_thread; ++i) {
                auto& node = nodes[(t * nodes_per_thread) + i];
                const bool keep = pass == 7 && (random() % 2) == 0;
                kept[(t * nodes_per_thread) + i] = keep ? 1 : 0;
                if (!keep) {
                    node.registration.disarm();
                }
            }
        }
    }));
    for (int round = 0; round < test_support::stress_iterations(100); ++round) {
        stepwright::cancel_source local;
        source = &local;
        for (auto& node : nodes) {
            node.calls.store(0, std::memory_order_relaxed);
        }
        rounds.run_round();
        EXPECT_TRUE(local.request_cancel());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            ASSERT_EQ(nodes[i].calls.load(), kept[i]) << "node " << i << " round " << round;
            nodes[i].registration.disarm();
        }
    }
}

// A request races arms and disarms on eight threads. Every node is invoked at
// most once; a node that stayed armed is invoked exactly once (inline when armed
// after the request); a node is never invoked after its disarm returned.
TEST(cancel_scaling, regression_request_races_arm_and_disarm) {
    std::vector<counted_node> nodes(thread_count * nodes_per_thread);
    std::vector<int> after_disarm(nodes.size(), -1);
    stepwright::cancel_source* source = nullptr;
    std::atomic<std::size_t> armed{0};
    test_support::race_rounds rounds(per_thread([&](std::size_t t) {
        const auto token = source->token();
        for (std::size_t i = 0; i < nodes_per_thread; ++i) {
            const std::size_t index = (t * nodes_per_thread) + i;
            nodes[index].registration.arm(token, &counted_node::call, &nodes[index]);
            armed.fetch_add(1, std::memory_order_relaxed);
            if (i % 2 == 1) {
                nodes[index - 1].registration.disarm();
                after_disarm[index - 1] = nodes[index - 1].calls.load(std::memory_order_relaxed);
            }
        }
    }));
    int invoked_before_disarm = 0;
    for (int round = 0; round < test_support::stress_iterations(300); ++round) {
        stepwright::cancel_source local;
        source = &local;
        for (auto& node : nodes) {
            node.calls.store(0, std::memory_order_relaxed);
        }
        armed.store(0, std::memory_order_relaxed);
        // Request at a different point of the arming in every round.
        const auto target = static_cast<std::size_t>(round * 37) % (nodes.size() + 1);
        rounds.run_round([&] {
            while (armed.load(std::memory_order_relaxed) < target) {
                std::this_thread::yield();
            }
            local.request_cancel();
        });
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const int calls = nodes[i].calls.load();
            if (i % 2 == 0) {
                ASSERT_LE(calls, 1);
                ASSERT_EQ(calls, after_disarm[i]) << "invoked after disarm returned";
                invoked_before_disarm += calls;
            } else {
                ASSERT_EQ(calls, 1) << "node " << i << " round " << round;
            }
            nodes[i].registration.disarm();
        }
    }
    std::printf("disarmed nodes invoked before their disarm: %d of %d\n", invoked_before_disarm,
                test_support::stress_iterations(300) * static_cast<int>(nodes.size() / 2));
}

// Callbacks deregister (and some rearm or destroy) themselves while other threads
// keep arming and disarming unrelated nodes on the same stripes.
TEST(cancel_scaling, regression_self_deregistration_under_contention) {
    struct self_node {
        std::unique_ptr<stepwright::cancel_registration> registration =
            std::make_unique<stepwright::cancel_registration>();
        stepwright::cancel_token next;
        std::atomic<int> calls{0};
        int kind = 0;
        static void call(void* ctx) noexcept {
            auto& self = *static_cast<self_node*>(ctx);
            self.calls.fetch_add(1, std::memory_order_relaxed);
            switch (self.kind) {
                case 0: // self-disarm, then keep the node
                    self.registration->disarm();
                    break;
                case 1: // self-destruction
                    self.registration.reset();
                    break;
                default: // self-rearm onto another token
                    self.registration->disarm();
                    self.registration->arm(self.next, &counted_node::call, nullptr);
                    break;
            }
        }
    };
    constexpr std::size_t self_count = 256;
    std::vector<self_node> selves(self_count);
    std::vector<counted_node> others(thread_count * nodes_per_thread);
    stepwright::cancel_source* source = nullptr;
    std::atomic<std::size_t> passes{0};
    test_support::race_rounds rounds(per_thread([&](std::size_t t) {
        // Stop once the request is visible: a node armed after it runs inline, and
        // each node runs at most once in all.
        const auto token = source->token();
        while (!token.cancel_requested()) {
            for (std::size_t i = 0; i < nodes_per_thread; ++i) {
                auto& node = others[(t * nodes_per_thread) + i];
                node.registration.arm(token, &counted_node::call, &node);
            }
            for (std::size_t i = 0; i < nodes_per_thread; ++i) {
                others[(t * nodes_per_thread) + i].registration.disarm();
            }
            passes.fetch_add(1, std::memory_order_relaxed);
        }
    }));
    stepwright::cancel_source second; // never requested: rearmed nodes stay quiet
    for (int round = 0; round < test_support::stress_iterations(100); ++round) {
        stepwright::cancel_source local;
        source = &local;
        for (std::size_t i = 0; i < self_count; ++i) {
            auto& node = selves[i];
            if (!node.registration) {
                node.registration = std::make_unique<stepwright::cancel_registration>();
            }
            node.kind = static_cast<int>(i % 3);
            node.next = second.token();
            node.calls.store(0, std::memory_order_relaxed);
            node.registration->arm(local.token(), &self_node::call, &node);
        }
        passes.store(0, std::memory_order_relaxed);
        rounds.run_round([&] {
            while (passes.load(std::memory_order_relaxed) < thread_count) {
                std::this_thread::yield();
            }
            local.request_cancel();
        });
        for (std::size_t i = 0; i < self_count; ++i) {
            ASSERT_EQ(selves[i].calls.load(), 1) << "node " << i;
            ASSERT_EQ(static_cast<bool>(selves[i].registration), i % 3 != 1);
        }
        for (auto& node : others) {
            ASSERT_LE(node.calls.exchange(0), 1);
        }
    }
    for (auto& node : selves) {
        if (node.registration) {
            node.registration->disarm();
        }
    }
}

// Foreign disarms race running callbacks: a disarm that returns must find the
// callback either not started or finished, never in progress.
TEST(cancel_scaling, regression_disarm_waits_for_running_callbacks_under_contention) {
    static std::atomic<int> invocations{0};
    struct slow_node {
        stepwright::cancel_registration registration;
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        unsigned seed = 1;
        static void call(void* ctx) noexcept {
            auto& self = *static_cast<slow_node*>(ctx);
            self.started.fetch_add(1, std::memory_order_relaxed);
            invocations.fetch_add(1, std::memory_order_relaxed);
            test_support::jitter(self.seed, 20);
            self.finished.fetch_add(1, std::memory_order_release);
        }
    };
    constexpr std::size_t per = 16;
    std::vector<slow_node> nodes(thread_count * per);
    std::atomic<int> torn{0};
    int target = 0;
    test_support::race_rounds rounds(per_thread([&](std::size_t t) {
        // Disarm while the requester is somewhere in the middle of its callbacks.
        while (invocations.load(std::memory_order_relaxed) < target) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < per; ++i) {
            auto& node = nodes[(t * per) + i];
            node.registration.disarm();
            if (node.started.load(std::memory_order_relaxed) !=
                node.finished.load(std::memory_order_acquire)) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }));
    for (int round = 0; round < test_support::stress_iterations(30); ++round) {
        stepwright::cancel_source local;
        invocations.store(0, std::memory_order_relaxed);
        target = 1 + round % 8;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].seed = static_cast<unsigned>((round * 1000) + static_cast<int>(i) + 1);
            nodes[i].started.store(0, std::memory_order_relaxed);
            nodes[i].finished.store(0, std::memory_order_relaxed);
            nodes[i].registration.arm(local.token(), &slow_node::call, &nodes[i]);
        }
        rounds.run_round([&] { local.request_cancel(); });
        for (auto& node : nodes) {
            ASSERT_LE(node.started.load(), 1);
        }
    }
    EXPECT_EQ(torn.load(), 0);
}

// Machines register lazily (at their first park): cancellation still reaches a
// park that comes after children and deferrals, a second park in the same run,
// and a park in a later run with a different token.
TEST(cancel_scaling, regression_lazy_machine_registration) {
    struct late_parker : stepwright::machine<late_parker> {
        test_support::sync_child child;
        int parks = 0;
        step begin() { return after<&late_parker::deferred_step>(child); }
        step deferred_step() { return defer<&late_parker::wait>(); }
        step wait() {
            ++parks;
            return park<&late_parker::woken>();
        }
        step woken(const stepwright::error& e) {
            if (e) {
                return fail(e);
            }
            return park<&late_parker::woken>(); // the second park of this run
        }
    };
    stepwright::manual_executor ex;
    late_parker op;
    op.bind(ex);
    for (int run = 0; run < 3; ++run) {
        stepwright::cancel_source source;
        test_support::receiver receiver;
        op.start(receiver.callback(), source.token());
        ex.run_all();
        if (run == 1) {
            op.wake(); // first park consumed; the machine parks again
            ex.run_all();
        }
        EXPECT_EQ(receiver.calls, 0);
        source.request_cancel();
        ex.run_all();
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::cancelled));
    }
    // A machine that never parks completes normally under a requested token.
    struct no_park : stepwright::machine<no_park> {
        step begin() { return done(); }
    } quick;
    stepwright::cancel_source requested;
    requested.request_cancel();
    test_support::receiver receiver;
    quick.start(receiver.callback(), requested.token());
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_FALSE(receiver.result);
}

// Many machines bound to a pool park on one shared token while the request
// arrives: every one of them completes, cancelled or woken, exactly once.
TEST(cancel_scaling, regression_shared_token_parks_race_request) {
    struct parker : stepwright::machine<parker> {
        step begin() { return defer<&parker::wait>(); }
        step wait() { return park<&parker::woken>(); }
        step woken(const stepwright::error& e) { return fail(e); }
    };
    constexpr std::size_t count = 256;
    stepwright::thread_pool pool(thread_count);
    std::vector<parker> machines(count);
    std::vector<test_support::receiver> receivers(count);
    for (auto& m : machines) {
        m.bind(pool);
    }
    int cancelled = 0;
    for (int round = 0; round < test_support::stress_iterations(40); ++round) {
        stepwright::cancel_source source;
        for (std::size_t i = 0; i < count; ++i) {
            receivers[i].calls = 0;
            receivers[i].completed.store(false, std::memory_order_relaxed);
            machines[i].start(receivers[i].callback(), source.token());
        }
        source.request_cancel();
        for (std::size_t i = 0; i < count; ++i) {
            // A machine that had not parked yet when the request came sees the
            // requested token when it parks.
            receivers[i].wait();
            EXPECT_EQ(receivers[i].calls, 1);
            EXPECT_TRUE(receivers[i].result.is(stepwright::errc::cancelled));
            cancelled += receivers[i].result.is(stepwright::errc::cancelled) ? 1 : 0;
        }
        pool.wait_idle();
    }
    EXPECT_EQ(cancelled, test_support::stress_iterations(40) * static_cast<int>(count));
}
