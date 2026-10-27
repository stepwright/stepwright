// A deeply recursive definition: length(n) = 1 + length(n - 1), length(0) = 0.
// This is equivalent to walking a linked list; no list allocation obscures it.
// What you gain:
// - An after(child) per node, but constant stack even without optimization.
// - Child operations can later become asynchronous without rewriting the loop.
// When to use the plain way instead:
// - An ordinary iterative loop is simpler and faster for synchronous traversal.
// - Recursion at a small, known depth is fine; steps cost a few ns each.
// Never run plain_length(10,000,000): at that depth nonoptimized recursion
// overflows a normal thread stack. We compare it at 1,000, then test the machine
// at the full depth. This is sequential child traversal, NOT ten million nested
// parent machines: arbitrary nested start calls are not flattened by the driver.
#include <stepwright/stepwright.hpp>

#include <cstdint>
#include <cstdio>

namespace {
std::uint64_t plain_length(std::uint64_t n) {
    if (n == 0) {
        return 0;
    }
    return 1 + plain_length(n - 1);
}
// Visit one node. It could await a page read; here it completes synchronously.
struct visit : stepwright::task {
    void on_start(stepwright::completion done, stepwright::cancel_token /*token*/) override {
        done({});
    }
};
struct walk : stepwright::machine<walk> {
    std::uint64_t depth = 0;
    std::uint64_t remaining = 0;
    std::uint64_t length = 0;
    visit child;
    step begin() {
        remaining = depth;
        length = 0;
        return then<&walk::next>();
    }
    step next() {
        if (remaining == 0) {
            return done();
        }
        if (cancel_requested()) {
            return fail(stepwright::errc::cancelled);
        }
        --remaining;
        return after<&walk::visited>(child);
    }
    step visited() {
        ++length;
        return then<&walk::next>();
    }
};
} // namespace

int main() {
    walk list;
    list.depth = 1'000;
    const auto small = stepwright::sync_wait(list);
    const bool match = !small && list.length == plain_length(list.depth);
    list.depth = 10'000'000;
    const auto deep = stepwright::sync_wait(list);
    const bool flat = !deep && list.length == list.depth;
    std::printf("small recursive results match: %s\n", match ? "yes" : "no");
    std::printf("constant-stack child visits: %llu\n",
                static_cast<unsigned long long>(list.length));
    return match && flat ? 0 : 1;
}
// expect: small recursive results match: yes
// expect: constant-stack child visits: 10000000
