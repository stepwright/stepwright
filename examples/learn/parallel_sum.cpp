// Sum a million integers: std::accumulate versus map/reduce over task objects.
// A second run models failed validation while other chunks await I/O.
// What you gain:
// - A container join owns addresses once; chunks can later wait without blocking.
// - First failure cancels siblings, and the join still waits for every completion.
// When to use the plain way instead:
// - A simple memory-bound sum often needs no scheduler or parallelism.
// - Steps cost a few ns; a numeric loop is cheaper per iteration.
#include <stepwright/stepwright.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <numeric>
#include <span>
#include <vector>

namespace {
std::int64_t plain_sum(const std::vector<std::int64_t>& values) {
    return std::accumulate(values.begin(), values.end(), std::int64_t{0});
}
struct chunk : stepwright::machine<chunk> {
    chunk(stepwright::thread_pool& pool, stepwright::event& io, std::span<const std::int64_t> input,
          std::atomic<int>& cancellations)
        : data(input)
        , cancelled(cancellations)
        , waiting(io) {
        bind(pool);
    }
    std::span<const std::int64_t> data;
    std::int64_t sum = 0;
    bool invalid = false;
    bool await_io = false;
    std::atomic<int>& cancelled;
    stepwright::event::waiter waiting;
    step begin() {
        sum = 0;
        return defer<&chunk::reduce>();
    }
    step reduce() {
        if (invalid) {
            return fail(std::errc::invalid_argument, "bad chunk");
        }
        if (await_io) {
            return forward(waiting);
        }
        if (cancel_requested()) {
            return fail(stepwright::errc::cancelled);
        }
        sum = std::accumulate(data.begin(), data.end(), std::int64_t{0});
        return done();
    }
    void on_complete(const stepwright::error& e) noexcept {
        if (e.is(stepwright::errc::cancelled)) {
            cancelled.fetch_add(1, std::memory_order_relaxed);
        }
    }
};
} // namespace

int main() {
    std::vector<std::int64_t> values(1'000'000);
    std::iota(values.begin(), values.end(), std::int64_t{1});
    const auto expected = plain_sum(values);
    stepwright::thread_pool pool(4);
    stepwright::event io;
    std::atomic<int> cancelled{0};
    std::deque<chunk> chunks;
    constexpr std::size_t count = 16;
    const std::span<const std::int64_t> data(values);
    for (std::size_t i = 0; i < count; ++i) {
        chunks.emplace_back(pool, io, data.subspan(i * (data.size() / count), data.size() / count),
                            cancelled);
    }
    stepwright::when_all_range all(chunks);
    const auto result = stepwright::sync_wait(all);
    std::int64_t sum = 0;
    for (const auto& c : chunks) {
        sum += c.sum;
    }
    const bool match = !result && sum == expected;
    for (auto& c : chunks) {
        c.await_io = true;
    }
    chunks[3].invalid = true;
    const auto failure = stepwright::sync_wait(all);
    pool.wait_idle();
    const bool stopped = failure.message() == "bad chunk" && cancelled.load() == 15;
    std::printf("parallel sum results match: %s\n", match ? "yes" : "no");
    std::printf("validation failed; waiting siblings cancelled: %d\n", cancelled.load());
    return match && stopped ? 0 : 1;
}
// expect: parallel sum results match: yes
// expect: validation failed; waiting siblings cancelled: 15
