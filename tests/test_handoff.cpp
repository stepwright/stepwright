#include <stepwright/detail/handoff.hpp>

#include <gtest/gtest.h>

#include "test_support.hpp"
#include <string>
#include <vector>

namespace {
constexpr std::uintptr_t max_depth = std::uintptr_t{64} * 1024;

struct chain_item : stepwright::work_item {
    chain_item* other = nullptr;
    int* remaining = nullptr;
    test_support::depth_probe* probe = nullptr;
};
void run_chain_item(stepwright::work_item* item) noexcept {
    auto& self = static_cast<chain_item&>(*item);
    self.probe->sample();
    if ((*self.remaining)-- > 0) {
        stepwright::detail::handoff(*self.other);
    }
}

struct named_item : stepwright::work_item {
    std::string name;
    std::vector<std::string>* log = nullptr;
    std::vector<named_item*> children;
};
void run_named(stepwright::work_item* item) noexcept {
    auto& self = static_cast<named_item&>(*item);
    self.log->push_back(self.name);
    for (auto* child : self.children) {
        stepwright::detail::handoff(*child);
    }
    self.log->push_back(self.name + "-end");
}
} // namespace

TEST(handoff, regression_stack_million_chain) {
    test_support::depth_probe probe;
    int remaining = test_support::stress_iterations(1'000'000);
    chain_item a;
    chain_item b;
    for (auto* item : {&a, &b}) {
        item->run = &run_chain_item;
        item->remaining = &remaining;
        item->probe = &probe;
    }
    a.other = &b;
    b.other = &a;
    stepwright::detail::handoff(a);
    EXPECT_EQ(remaining, -1);
    EXPECT_LT(probe.span(), max_depth);
}

TEST(handoff, nested_completions_run_in_fifo_order_after_the_current_one) {
    std::vector<std::string> log;
    named_item a;
    named_item b;
    named_item c;
    named_item d;
    for (auto [item, name] : {std::pair{&a, "a"}, {&b, "b"}, {&c, "c"}, {&d, "d"}}) {
        item->run = &run_named;
        item->name = name;
        item->log = &log;
    }
    a.children = {&b, &c};
    b.children = {&d};
    stepwright::detail::handoff(a);
    const std::vector<std::string> expected{"a", "a-end", "b", "b-end", "c", "c-end", "d", "d-end"};
    EXPECT_EQ(log, expected);
    EXPECT_FALSE(stepwright::detail::handoff_state().active);
}

TEST(handoff, scope_defers_until_outermost_scope_ends) {
    std::vector<std::string> log;
    named_item a;
    a.run = &run_named;
    a.name = "a";
    a.log = &log;
    {
        const stepwright::detail::handoff_scope outer;
        {
            const stepwright::detail::handoff_scope inner;
            stepwright::detail::handoff(a);
        }
        EXPECT_TRUE(log.empty());
    }
    EXPECT_EQ(log.size(), 2U);
    // Outside any scope delivery is immediate.
    stepwright::detail::handoff(a);
    EXPECT_EQ(log.size(), 4U);
}

TEST(handoff, regression_stack_event_ping_pong_million) {
    // Each completion re-arms its own waiter and sets the peer's event, so every
    // completion is caused by the previous one: a chain through real primitives.
    struct side {
        stepwright::event ev;
        stepwright::event::waiter waiter{ev};
        side* peer = nullptr;
        int* remaining = nullptr;
        test_support::depth_probe* probe = nullptr;
        void arm() { waiter.start(stepwright::completion::bind<&side::fired>(this)); }
        void fired(const stepwright::error&) noexcept {
            probe->sample();
            ev.reset();
            if ((*remaining)-- > 0) {
                arm();
                peer->ev.set();
            }
        }
    };
    test_support::depth_probe probe;
    int remaining = test_support::stress_iterations(1'000'000);
    side a;
    side b;
    a.peer = &b;
    b.peer = &a;
    for (auto* s : {&a, &b}) {
        s->remaining = &remaining;
        s->probe = &probe;
    }
    a.arm();
    b.arm();
    a.ev.set();
    EXPECT_LT(remaining, 0);
    EXPECT_LT(probe.span(), max_depth);
    // One side is still waiting after the chain ends; release it.
    if (!a.ev.is_set()) {
        remaining = 0;
        a.ev.set();
    }
    if (!b.ev.is_set()) {
        remaining = 0;
        b.ev.set();
    }
}
