#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <latch>
#include <thread>

TEST(machine, regression_cross_thread_child_completion) {
    struct op_type : stepwright::machine<op_type> {
        test_support::async_child child;
        std::thread::id resumed;
        step begin() { return after<&op_type::finish>(child); }
        step finish(const stepwright::error& e) {
            resumed = std::this_thread::get_id();
            return fail(e);
        }
    } op;
    test_support::receiver receiver;
    op.start(receiver.callback());
    std::thread::id delivery;
    std::thread thread([&] {
        delivery = std::this_thread::get_id();
        op.child.finish();
    });
    thread.join();
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_EQ(op.resumed, delivery);
}
TEST(machine, regression_wake_before_and_after_park) {
    std::barrier rendezvous(2);
    struct racing : stepwright::machine<racing> {
        std::barrier<>* rendezvous = nullptr;
        int delay = 0;
        int value = 0;
        step begin() {
            rendezvous->arrive_and_wait();
            for (int i = 0; i < delay; ++i) {
                std::this_thread::yield();
            }
            return park<&racing::finish>();
        }
        step finish() {
            value = 42;
            return done();
        }
    } op;
    op.rendezvous = &rendezvous;
    std::thread deliverer([&] {
        std::uint32_t random = 42;
        for (int i = 0; i < test_support::stress_iterations(10'000); ++i) {
            rendezvous.arrive_and_wait();
            random = random * 1664525U + 1013904223U;
            for (std::uint32_t j = 0; j < (random % 4); ++j) {
                std::this_thread::yield();
            }
            op.wake();
            rendezvous.arrive_and_wait();
        }
    });
    for (int i = 0; i < test_support::stress_iterations(10'000); ++i) {
        test_support::receiver receiver;
        op.delay = i % 4;
        op.value = 0;
        op.start(receiver.callback());
        receiver.wait();
        rendezvous.arrive_and_wait();
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_EQ(op.value, 42);
    }
    deliverer.join();
}
TEST(machine, regression_concurrent_duplicate_wakes) {
    struct racing : stepwright::machine<racing> {
        int calls = 0;
        step begin() {
            std::array<std::thread, 4> threads;
            for (auto& thread : threads) {
                thread = std::thread(
                    [this] { wake(stepwright::error::make(stepwright::errc::closed)); });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            return park<&racing::finish>();
        }
        step finish(const stepwright::error& e) {
            ++calls;
            return fail(e);
        }
    } op;
    for (int i = 0; i < test_support::stress_iterations(500); ++i) {
        test_support::receiver receiver;
        op.start(receiver.callback());
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
    }
    EXPECT_EQ(op.calls, test_support::stress_iterations(500));
}
TEST(machine, bound_affinity_and_inline_home) {
    struct op_type : stepwright::machine<op_type> {
        std::thread::id resumed;
        step begin() { return park<&op_type::finish>(); }
        step finish() {
            resumed = std::this_thread::get_id();
            return done();
        }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    EXPECT_EQ(op.home(), &ex);
    test_support::receiver receiver;
    op.start(receiver.callback());
    std::thread foreign([&] { op.wake(); });
    foreign.join();
    EXPECT_EQ(receiver.calls, 0);
    EXPECT_EQ(ex.pending(), 1U);
    std::thread::id home_thread;
    std::thread runner([&] {
        home_thread = std::this_thread::get_id();
        ex.run_all();
    });
    runner.join();
    EXPECT_EQ(op.resumed, home_thread);
    EXPECT_EQ(receiver.calls, 1);
    stepwright::inline_executor inline_ex;
    op.bind(inline_ex);
    test_support::receiver inline_receiver;
    op.start(inline_receiver.callback());
    op.wake();
    EXPECT_EQ(inline_receiver.calls, 1);
}
TEST(machine, regression_duplicate_wakes_after_suspension) {
    struct racing : stepwright::machine<racing> {
        std::latch* release = nullptr;
        int steps = 0;
        step begin() { return park<&racing::finish>(); }
        step finish() {
            ++steps;
            release->wait();
            return done();
        }
    } op;
    for (int i = 0; i < test_support::stress_iterations(200); ++i) {
        std::latch release(1);
        std::barrier start(4);
        std::atomic<int> returned{0};
        op.release = &release;
        test_support::receiver receiver;
        op.start(receiver.callback());
        std::array<std::thread, 4> threads;
        for (auto& thread : threads) {
            thread = std::thread([&] {
                start.arrive_and_wait();
                op.wake();
                returned.fetch_add(1, std::memory_order_release);
            });
        }
        while (returned.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        release.count_down();
        for (auto& thread : threads) {
            thread.join();
        }
        EXPECT_EQ(receiver.calls, 1);
    }
    EXPECT_EQ(op.steps, test_support::stress_iterations(200));
}

namespace {
// Four persistent producers, each with a single mailbox. A producer snapshots
// its callback and frees the mailbox before invoking user code; a continuation
// may already post another item before that callback returns.
class completion_pool {
    struct mailbox {
        std::atomic<bool> ready{false};
        stepwright::completion done;
        int sequence = 0;
    };

public:
    completion_pool() {
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            threads_[i] = std::thread([this, i] {
                for (;;) {
                    if (!boxes_[i].ready.load(std::memory_order_acquire)) {
                        if (stop_.load(std::memory_order_acquire)) {
                            return;
                        }
                        std::this_thread::yield();
                        continue;
                    }
                    const auto done = boxes_[i].done;
                    const int sequence = boxes_[i].sequence;
                    boxes_[i].ready.store(false, std::memory_order_release);
                    completed_sequence = sequence; // Published through the engine, not an atomic.
                    completion_threads.fetch_or(1U << i, std::memory_order_relaxed);
                    const auto result = sequence % 17 == 0 ? failure_ : stepwright::error{};
                    done(result);
                }
            });
        }
    }
    ~completion_pool() {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            thread.join();
        }
    }
    completion_pool(const completion_pool&) = delete;
    completion_pool& operator=(const completion_pool&) = delete;
    void post(stepwright::completion done, int sequence) {
        auto& box = boxes_[static_cast<std::size_t>(sequence) % boxes_.size()];
        EXPECT_FALSE(box.ready.load(std::memory_order_acquire));
        box.done = done;
        box.sequence = sequence;
        box.ready.store(true, std::memory_order_release);
    }
    int completed_sequence = 0;
    std::atomic<unsigned> completion_threads{0};

private:
    std::array<mailbox, 4> boxes_;
    std::array<std::thread, 4> threads_;
    std::atomic<bool> stop_{false};
    stepwright::error failure_ = stepwright::error::make(stepwright::errc::closed);
};

void run_cross_thread_chain(bool bound) {
    completion_pool pool;
    struct child_type : stepwright::task {
        completion_pool* pool = nullptr;
        int sequence = 0;
        void on_start(stepwright::completion done, stepwright::cancel_token) override {
            pool->post(done, ++sequence);
        }
    };
    struct chain : stepwright::machine<chain> {
        child_type child;
        int steps = 0;
        int length = 0;
        bool ordered = true;
        bool affine = true;
        int thread_switches = 0;
        std::thread::id previous_thread;
        std::thread::id executor_thread;
        step begin() { return after<&chain::on_child>(child); }
        step on_child(const stepwright::error& e) {
            ++steps;
            const auto current = std::this_thread::get_id();
            if (steps > 1 && current != previous_thread) {
                ++thread_switches;
            }
            previous_thread = current;
            const bool expected_failure = steps % 17 == 0;
            ordered = ordered && static_cast<bool>(e) == expected_failure &&
                      (!e || e.is(stepwright::errc::closed)) &&
                      child.pool->completed_sequence == steps;
            if (home()) {
                affine = affine && home()->running_in_this_thread() &&
                         std::this_thread::get_id() == executor_thread;
            }
            return steps == length ? done() : after<&chain::on_child>(child);
        }
    } op;
    op.child.pool = &pool;
    op.length = test_support::stress_iterations(100'000);
    test_support::receiver receiver;
    stepwright::manual_executor ex;
    std::thread runner;
    if (bound) {
        op.bind(ex);
        ex.post([&] {
            op.executor_thread = std::this_thread::get_id();
            op.start(receiver.callback());
        });
        runner = std::thread([&] {
            while (!receiver.completed.load(std::memory_order_acquire)) {
                if (!ex.run_one()) {
                    std::this_thread::yield();
                }
            }
        });
    } else {
        op.start(receiver.callback());
    }
    receiver.wait();
    if (runner.joinable()) {
        runner.join();
    }
    EXPECT_EQ(receiver.calls, 1);
    EXPECT_EQ(op.steps, op.length);
    EXPECT_EQ(op.child.sequence, op.length);
    EXPECT_TRUE(op.ordered);
    EXPECT_TRUE(op.affine);
    EXPECT_EQ(pool.completion_threads.load(), 15U);
    if (bound) {
        EXPECT_EQ(op.thread_switches, 0);
    } else {
        EXPECT_GT(op.thread_switches, 0);
    }
    ::testing::Test::RecordProperty("thread_switches", op.thread_switches);
}
} // namespace

TEST(machine, cross_thread_chain_unbound) {
    run_cross_thread_chain(false);
}
TEST(machine, cross_thread_chain_bound) {
    run_cross_thread_chain(true);
}

namespace {
void run_reply_during_child_wait(bool bound) {
    struct request : stepwright::machine<request> {
        test_support::async_child send;
        int sent_steps = 0;
        int replies = 0;
        bool reply_correct = true;
        bool affine = true;
        step begin() { return after<&request::on_sent>(send); }
        step on_sent() {
            ++sent_steps;
            if (home()) {
                affine = affine && home()->running_in_this_thread();
            }
            return park<&request::on_reply>();
        }
        step on_reply(const stepwright::error& e) {
            ++replies;
            reply_correct =
                reply_correct && e.is(stepwright::errc::closed) && e.message() == "reply";
            if (home()) {
                affine = affine && home()->running_in_this_thread();
            }
            return fail(e);
        }
    } op;
    stepwright::manual_executor home;
    std::atomic<int> started{0};
    std::atomic<int> epoch{0};
    std::atomic<int> sends_finished{0};
    std::atomic<int> wakes_finished{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> home_stop{false};
    std::barrier rendezvous(bound ? 2 : 3);
    const auto reply = stepwright::error::make(stepwright::errc::closed, "reply");

    struct start_item : stepwright::work_item {
        request* op = nullptr;
        test_support::receiver* receiver = nullptr;
        std::atomic<int>* started = nullptr;
        std::atomic<int>* sends_finished = nullptr;
        std::atomic<int>* wakes_finished = nullptr;
        int iteration = 0;
        start_item() {
            run = [](stepwright::work_item* item) noexcept {
                auto& node = *static_cast<start_item*>(item);
                node.op->start(node.receiver->callback());
                node.started->store(node.iteration, std::memory_order_release);
                // Hold the home thread outside the machine until the peer has
                // completed send AND woken the reply. This forces the reply to
                // precede processing of the queued child completion every time.
                while (node.sends_finished->load(std::memory_order_acquire) != node.iteration ||
                       node.wakes_finished->load(std::memory_order_acquire) != node.iteration) {
                    std::this_thread::yield();
                }
            };
        }
    } starter;
    starter.op = &op;
    starter.started = &started;
    starter.sends_finished = &sends_finished;
    starter.wakes_finished = &wakes_finished;
    std::thread home_thread;
    if (bound) {
        op.bind(home);
        home_thread = std::thread([&] {
            while (!home_stop.load(std::memory_order_acquire)) {
                if (!home.run_one()) {
                    std::this_thread::yield();
                }
            }
        });
    }
    std::thread sender([&] {
        for (;;) {
            rendezvous.arrive_and_wait();
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            const int iteration = epoch.load(std::memory_order_acquire);
            op.send.finish();
            if (bound) {
                op.wake(reply); // Same peer, immediately after send completion.
                wakes_finished.store(iteration, std::memory_order_release);
            }
            sends_finished.store(iteration, std::memory_order_release);
        }
    });
    std::thread reply_thread;
    if (!bound) {
        reply_thread = std::thread([&] {
            for (;;) {
                rendezvous.arrive_and_wait();
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                const int iteration = epoch.load(std::memory_order_acquire);
                op.wake(reply); // Races send completion from another producer.
                wakes_finished.store(iteration, std::memory_order_release);
            }
        });
    }
    const auto await = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ADD_FAILURE() << "reply race stalled (machine or producer did not advance)";
                // A broken engine can leave active work that cannot be safely
                // destroyed. Fail fast rather than hang or unwind live threads.
                std::abort();
            }
            std::this_thread::yield();
        }
    };
    const int iterations = test_support::stress_iterations(20'000);
    for (int i = 1; i <= iterations; ++i) {
        test_support::receiver receiver;
        if (bound) {
            starter.receiver = &receiver;
            starter.iteration = i;
            home.post(starter);
            await([&] { return started.load(std::memory_order_acquire) == i; });
        } else {
            op.start(receiver.callback());
        }
        // Explicit publication of the receiver/start for older libc++/TSan
        // barrier implementations; finished counters also guard reuse/lifetime.
        epoch.store(i, std::memory_order_release);
        rendezvous.arrive_and_wait();
        await([&] {
            return receiver.completed.load(std::memory_order_acquire) &&
                   sends_finished.load(std::memory_order_acquire) == i &&
                   wakes_finished.load(std::memory_order_acquire) == i;
        });
        EXPECT_EQ(receiver.calls, 1);
        EXPECT_TRUE(receiver.result.is(stepwright::errc::closed));
        EXPECT_EQ(op.sent_steps, i);
        EXPECT_EQ(op.replies, i);
    }
    stop.store(true, std::memory_order_release);
    rendezvous.arrive_and_wait();
    sender.join();
    if (reply_thread.joinable()) {
        reply_thread.join();
    }
    home_stop.store(true, std::memory_order_release);
    if (home_thread.joinable()) {
        home_thread.join();
    }
    EXPECT_TRUE(op.reply_correct);
    EXPECT_TRUE(op.affine);
    EXPECT_EQ(home.pending(), 0U);
    ::testing::Test::RecordProperty("iterations", iterations);
    ::testing::Test::RecordProperty("replies", op.replies);
}
} // namespace

TEST(machine, regression_reply_during_child_wait_bound) {
    run_reply_during_child_wait(true);
}
TEST(machine, regression_reply_during_child_wait_unbound) {
    run_reply_during_child_wait(false);
}
