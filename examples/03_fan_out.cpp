// 03: fan-out and fan-in with when_all on a thread_pool.
//
// when_all starts every child with its own cancellation token and completes once
// *all* children have completed (never earlier: no child outlives the fan-out).
// With the default policy, cancel_on_error, the first failure wins: it becomes
// the result and requests cancellation of the siblings still running.
//
// Each shard below is a machine bound to the pool. Its first step defers, so the
// shards spread over the workers; then it waits for its "download" (a timer).
// A waiting shard is cancelled by the failure of another one: its delay
// completes with errc::cancelled, and so does the shard.
#include <stepwright/stepwright.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>

using namespace std::chrono_literals;

namespace example {
constexpr int shard_count = 8;

struct fetch_shard : stepwright::machine<fetch_shard> {
    fetch_shard(stepwright::thread_pool& pool, stepwright::timer_service& timers)
        : download_(timers, 10s) {
        bind(pool);
    }
    int index = 0;
    int corrupt_index = -1; // which shard fails in this run (-1: none)
    int bytes = 0;
    std::atomic<int>* cancelled = nullptr;

    step begin() {
        bytes = 0;
        return defer<&fetch_shard::start_download>(); // continue on a pool worker
    }
    step start_download() {
        if (index == corrupt_index) {
            return fail(std::errc::illegal_byte_sequence, "corrupt block");
        }
        if (corrupt_index < 0) {
            bytes = 1000 + index; // the happy path: no waiting
            return done();
        }
        return after<&fetch_shard::on_downloaded>(download_);
    }
    step on_downloaded(const stepwright::error& e) { // inspect cancellation to count it
        if (e.is(stepwright::errc::cancelled)) {
            cancelled->fetch_add(1, std::memory_order_relaxed);
        }
        return fail(e);
    }

private:
    stepwright::delay download_;
};
} // namespace example

int main() {
    stepwright::steady_timer_service timers;
    stepwright::thread_pool pool(4);
    std::atomic<int> cancelled{0};

    std::deque<example::fetch_shard> shards; // machines have identity: deque never moves them
    for (int i = 0; i < example::shard_count; ++i) {
        shards.emplace_back(pool, timers);
        shards.back().index = i;
        shards.back().cancelled = &cancelled;
    }
    stepwright::when_all_range all(shards); // copies addresses once; cancel_on_error by default

    // Run 1: every shard succeeds.
    stepwright::error e = stepwright::sync_wait(all);
    int total = 0;
    for (const auto& shard : shards) {
        total += shard.bytes;
    }
    std::printf("run 1: %s, %d shards, %d bytes\n", e ? "failed" : "ok", example::shard_count,
                total);

    // Run 2: shard 3 is corrupt. The others are waiting for their downloads and
    // get cancelled; when_all reports shard 3's error, after all eight are done.
    for (auto& shard : shards) {
        shard.corrupt_index = 3;
    }
    e = stepwright::sync_wait(all);
    std::printf("run 2: failed with '%.*s' from %s\n", static_cast<int>(e.message().size()),
                e.message().data(), e.context().empty() ? "?" : e.context()[0]);
    std::printf("run 2: %d siblings cancelled\n", cancelled.load());

    pool.wait_idle();
    const bool ok = total == 8 * 1000 + 28 && e.message() == "corrupt block" &&
                    cancelled.load() == example::shard_count - 1;
    return ok ? 0 : 1;
}

// expect: run 1: ok, 8 shards, 8028 bytes
// expect: run 2: failed with 'corrupt block' from example::fetch_shard
// expect: run 2: 7 siblings cancelled
