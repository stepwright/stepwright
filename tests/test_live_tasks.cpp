// Compiled with STEPWRIGHT_TRACK_TASKS=1 for this executable only (see
// tests/CMakeLists.txt): every stepwright translation unit of this program agrees.
#include <stepwright/debug/live_tasks.hpp>

#include <gtest/gtest.h>

#include "counting_allocator.hpp"
#include "test_support.hpp"
#include <atomic>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static_assert(stepwright::debug::tracking_enabled);

namespace {
using stepwright::debug::live_task;
using stepwright::debug::task_state;

std::vector<live_task> live_matching(const std::string& needle) {
    std::vector<live_task> found;
    stepwright::debug::for_each_live([&](const live_task& task) {
        if (std::string(task.type_name).find(needle) != std::string::npos) {
            found.push_back(task);
        }
    });
    return found;
}

struct parked_op : stepwright::machine<parked_op> {
    step begin() { return park<&parked_op::on_wake>(); }
    step on_wake(const stepwright::error& e) { return fail(e); }
};

struct child_op : stepwright::machine<child_op> {
    stepwright::task* child = nullptr;
    step begin() { return after<&child_op::on_child>(*child); }
    step on_child() { return done(); }
};

struct forward_op : stepwright::machine<forward_op> {
    stepwright::task* child = nullptr;
    step begin() { return forward(*child); }
};

struct deferred_op : stepwright::machine<deferred_op> {
    step begin() { return defer<&deferred_op::later>(); }
    step later() { return done(); }
};

struct self_inspecting_op : stepwright::machine<self_inspecting_op> {
    std::vector<live_task> seen;
    step begin() {
        seen = live_matching("self_inspecting_op");
        return done();
    }
};

struct counter_source : stepwright::source_machine<counter_source, int> {
    int next = 0;
    int limit = 3;
    step begin() { return next == limit ? end() : emit<&counter_source::begin>(next++); }
};
} // namespace

TEST(live_tasks, machines_appear_while_live_and_disappear_after_completion) {
    EXPECT_EQ(stepwright::debug::live_count(), 0U);

    parked_op parked;
    test_support::receiver parked_done;
    const auto before = std::chrono::steady_clock::now();
    parked.start(parked_done.callback());
    auto found = live_matching("parked_op");
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].state, task_state::parked);
    ASSERT_NE(found[0].resume_step, nullptr);
    EXPECT_NE(std::string(found[0].resume_step).find("parked_op::on_wake"), std::string::npos);
    EXPECT_EQ(found[0].thread, std::this_thread::get_id());
    EXPECT_GE(found[0].started, before);

    test_support::async_child child;
    child_op waiting;
    waiting.child = &child;
    test_support::receiver waiting_done;
    waiting.start(waiting_done.callback());
    found = live_matching("child_op");
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].state, task_state::waiting_child);
    EXPECT_NE(std::string(found[0].resume_step).find("child_op::on_child"), std::string::npos);
    EXPECT_EQ(stepwright::debug::live_count(), 2U);

    parked.wake();
    EXPECT_EQ(parked_done.calls, 1);
    EXPECT_TRUE(live_matching("parked_op").empty());
    child.finish();
    EXPECT_EQ(waiting_done.calls, 1);
    EXPECT_EQ(stepwright::debug::live_count(), 0U);

    // Restart registers again.
    parked.start(parked_done.callback());
    EXPECT_EQ(live_matching("parked_op").size(), 1U);
    parked.wake();
    EXPECT_EQ(stepwright::debug::live_count(), 0U);
}

TEST(live_tasks, running_forwarding_and_deferred_states) {
    self_inspecting_op self;
    test_support::receiver self_done;
    self.start(self_done.callback());
    ASSERT_EQ(self.seen.size(), 1U);
    EXPECT_EQ(self.seen[0].state, task_state::running);
    EXPECT_EQ(self.seen[0].resume_step, nullptr);

    test_support::async_child child;
    forward_op forwarding;
    forwarding.child = &child;
    test_support::receiver forward_done;
    forwarding.start(forward_done.callback());
    auto found = live_matching("forward_op");
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].state, task_state::waiting_child);
    EXPECT_EQ(found[0].resume_step, nullptr);
    child.finish();

    stepwright::manual_executor ex;
    deferred_op deferred;
    deferred.bind(ex);
    test_support::receiver deferred_done;
    deferred.start(deferred_done.callback());
    found = live_matching("deferred_op");
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].state, task_state::deferred);
    EXPECT_NE(std::string(found[0].resume_step).find("deferred_op::later"), std::string::npos);
    ex.run_all();
    EXPECT_EQ(deferred_done.calls, 1);
    EXPECT_EQ(stepwright::debug::live_count(), 0U);
}

TEST(live_tasks, sources_register_from_first_pull_until_end_or_destruction) {
    {
        counter_source source;
        EXPECT_TRUE(live_matching("counter_source").empty()); // lazy: nothing ran
        EXPECT_EQ(source.try_pull().value(), 0);
        auto found = live_matching("counter_source");
        ASSERT_EQ(found.size(), 1U);
        EXPECT_EQ(found[0].state, task_state::suspended);
        EXPECT_NE(std::string(found[0].resume_step).find("counter_source::begin"),
                  std::string::npos);
        EXPECT_EQ(source.try_pull().value(), 1);
        EXPECT_EQ(source.try_pull().value(), 2);
        EXPECT_TRUE(source.try_pull().at_end());
        EXPECT_TRUE(live_matching("counter_source").empty());
    }
    {
        counter_source source;
        EXPECT_EQ(source.try_pull().value(), 0);
        EXPECT_EQ(live_matching("counter_source").size(), 1U);
    } // destroyed between pulls
    EXPECT_EQ(stepwright::debug::live_count(), 0U);
}

TEST(live_tasks, dump_format) {
    parked_op parked;
    test_support::receiver done;
    parked.start(done.callback());
    test_support::async_child child;
    child_op waiting;
    waiting.child = &child;
    waiting.start(done.callback());
    std::ostringstream out;
    EXPECT_EQ(stepwright::debug::dump(out), 2U);
    std::cout << out.str();
    const std::regex line(R"(^\S.* state=(parked|waiting_child) step=&.+ age=\d+us thread=\S+$)");
    std::istringstream lines(out.str());
    std::string text;
    int count = 0;
    while (std::getline(lines, text)) {
        EXPECT_TRUE(std::regex_match(text, line)) << text;
        ++count;
    }
    EXPECT_EQ(count, 2);
    EXPECT_NE(out.str().find("parked_op state=parked step=&"), std::string::npos);
    EXPECT_NE(out.str().find("child_op state=waiting_child step=&"), std::string::npos);
    parked.wake();
    child.finish();
    std::ostringstream empty;
    EXPECT_EQ(stepwright::debug::dump(empty), 0U);
    EXPECT_TRUE(empty.str().empty());
}

TEST(live_tasks, registration_does_not_allocate) {
    parked_op parked;
    test_support::receiver done;
    parked.start(done.callback()); // warm up the registry and thread-locals
    parked.wake();
    const auto allocated = count_allocations([&] {
        for (int i = 0; i < 100; ++i) {
            parked.start(done.callback());
            parked.wake();
        }
    });
    EXPECT_EQ(allocated, 0U);
    EXPECT_EQ(done.calls, 101);
}

TEST(live_tasks, concurrent_registration_and_dumps) {
    const int per_thread = test_support::stress_iterations(5'000);
    constexpr int threads = 4;
    stepwright::thread_pool pool(4);
    std::atomic<bool> stop{false};
    std::atomic<long> dumped{0};
    std::thread dumper([&] {
        while (!stop.load(std::memory_order_acquire)) {
            std::ostringstream out;
            dumped.fetch_add(static_cast<long>(stepwright::debug::dump(out)));
        }
    });
    std::vector<std::thread> workers;
    workers.reserve(threads);
    std::atomic<long> completed{0};
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            deferred_op op;
            op.bind(pool);
            for (int i = 0; i < per_thread; ++i) {
                test_support::receiver done;
                op.start(done.callback());
                done.wait();
                completed.fetch_add(1);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    stop.store(true, std::memory_order_release);
    dumper.join();
    std::cout << "completed=" << completed.load() << " dumped lines=" << dumped.load() << '\n';
    EXPECT_EQ(completed.load(), static_cast<long>(threads) * per_thread);
    EXPECT_EQ(stepwright::debug::live_count(), 0U);
}

TEST(live_tasks, layout_grows_only_with_tracking) {
    struct sample : stepwright::machine<sample> {
        step begin() { return done(); }
    };
    std::cout << "tracked machine=" << sizeof(sample) << " step=" << sizeof(stepwright::step)
              << " live_node=" << sizeof(stepwright::detail::live_node) << '\n';
    EXPECT_EQ(sizeof(sample), 112U + sizeof(stepwright::detail::live_node));
}
