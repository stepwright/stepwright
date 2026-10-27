#include <stepwright/stepwright.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <span>
#include <vector>

namespace {
struct sync_child : stepwright::task {
    void on_start(stepwright::completion done, stepwright::cancel_token) override { done({}); }
};
constexpr stepwright::completion ignored{.fn = [](void*, const stepwright::error& result) noexcept {
    bool failed = static_cast<bool>(result);
    benchmark::DoNotOptimize(failed);
}};
struct handwritten : stepwright::machine<handwritten> {
    explicit handwritten(std::span<stepwright::task* const> tasks) : children(tasks) {}
    std::span<stepwright::task* const> children;
    std::size_t index = 0;
    step begin() {
        index = 0;
        return then<&handwritten::next>();
    }
    step next() {
        if (index == children.size())
            return done();
        if (cancel_requested())
            return fail(stepwright::errc::cancelled);
        return after<&handwritten::next>(*children[index++]);
    }
};
template <bool Library>
void sequence_benchmark(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<sync_child> children(count);
    std::vector<stepwright::task*> addresses;
    for (auto& child : children)
        addresses.push_back(&child);
    using operation = std::conditional_t<Library, stepwright::sequence_range, handwritten>;
    operation sequence(addresses);
    for (auto _ : state)
        sequence.start(ignored);
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
void BM_sequence(benchmark::State& state) {
    sequence_benchmark<true>(state);
}
void BM_handwritten_sequence(benchmark::State& state) {
    sequence_benchmark<false>(state);
}
void BM_sync_wait_synchronous(benchmark::State& state) {
    sync_child child;
    for (auto _ : state) {
        const auto result = stepwright::sync_wait(child);
        bool failed = static_cast<bool>(result);
        benchmark::DoNotOptimize(failed);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_sequence)->Arg(4)->Arg(64)->Arg(1024);
BENCHMARK(BM_handwritten_sequence)->Arg(4)->Arg(64)->Arg(1024);
BENCHMARK(BM_sync_wait_synchronous);
} // namespace
