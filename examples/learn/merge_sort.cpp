// Sort a million seeded integers: std::sort versus a bounded parallel merge tree.
// Recursive std::async can create an unbounded number of threads/futures. Here
// the tree and scratch storage are built once; sorting itself allocates nothing.
// What you gain:
// - Independent halves run on a fixed pool, then a structured join permits merge.
// - The start token propagates through the tree; no child outlives its parent.
// When to use the plain way instead:
// - Small arrays or workloads where memory traffic dominates parallelism.
// - Pure computation is not inherently faster with steps (a few ns overhead each).
#include <stepwright/stepwright.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace {
void plain_sort(std::vector<int>& values) {
    std::ranges::sort(values);
}
struct sort_tree : stepwright::machine<sort_tree> {
    sort_tree(stepwright::thread_pool& pool, std::span<int> input, std::span<int> buffer)
        : values(input)
        , scratch(buffer) {
        bind(pool);
        if (values.size() > 32'768) {
            const auto half = values.size() / 2;
            left = std::make_unique<sort_tree>(pool, values.first(half), scratch.first(half));
            right = std::make_unique<sort_tree>(pool, values.subspan(half), scratch.subspan(half));
            halves.emplace(*left, *right);
        }
    }
    std::span<int> values, scratch;
    std::unique_ptr<sort_tree> left, right;
    std::optional<stepwright::when_all<2>> halves; // only interior nodes have a join
    step begin() { return defer<&sort_tree::sort>(); }
    step sort() {
        if (cancel_requested()) {
            return fail(stepwright::errc::cancelled);
        }
        if (!halves) {
            std::ranges::sort(values); // cancellation is cooperative, not preemptive
            return done();
        }
        return after<&sort_tree::merge>(*halves);
    }
    step merge() {
        if (cancel_requested()) {
            return fail(stepwright::errc::cancelled);
        }
        std::merge(left->values.begin(), left->values.end(), right->values.begin(),
                   right->values.end(), scratch.begin());
        std::ranges::copy(scratch, values.begin());
        return done();
    }
};
} // namespace

int main() {
    std::vector<int> values(1'000'000);
    std::mt19937 random(42);
    for (auto& value : values) {
        value = static_cast<int>(random() & 0x7fffffffU);
    }
    auto expected = values;
    plain_sort(expected);
    std::vector<int> scratch(values.size());
    stepwright::thread_pool pool(4);
    sort_tree sort(pool, values, scratch); // all allocations precede start
    const auto result = stepwright::sync_wait(sort);
    pool.wait_idle();
    const bool match = !result && values == expected;
    std::printf("million-element sort results match: %s\n", match ? "yes" : "no");
    std::puts("parallel halves use four workers and prebuilt storage");
    return match ? 0 : 1;
}
// expect: million-element sort results match: yes
// expect: parallel halves use four workers and prebuilt storage
