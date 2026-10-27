// 05: a pipeline with backpressure: producers -> bounded channels -> merge ->
// for_each.
//
// A channel<T> is a multi-producer, single-consumer queue that is also a
// source<T> (a pull stream). A *bounded* channel gives backpressure: when it is
// full, try_send() refuses and a send() task waits (without a thread) until the
// consumer makes room. merge() pulls fairly from several sources, and
// for_each() drains a source into a callback, as a task.
//
// Part 1 shows the backpressure on one thread, step by step. Part 2 runs the
// real pipeline on a thread pool: four producer machines, two channels of eight
// slots, one merge, one consumer.
#include <stepwright/stepwright.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>

namespace example {
// Sends first, first + 1, ..., first + count - 1, waiting for room when full.
struct producer : stepwright::machine<producer> {
    producer(stepwright::thread_pool& pool, stepwright::channel<std::uint64_t>& out,
             std::uint64_t first, std::uint64_t count)
        : next_(first)
        , end_(first + count)
        , send_(out) {
        bind(pool);
    }
    step begin() {
        if (next_ == end_) {
            return done();
        }
        return after<&producer::begin>(send_.with(next_++));
    }

private:
    std::uint64_t next_;
    std::uint64_t end_;
    stepwright::channel<std::uint64_t>::send_op send_; // stable, reusable child
};
} // namespace example

int main() {
    // --- Part 1: backpressure, one step at a time ---------------------------------
    stepwright::channel<int> small(2);
    const bool first = small.try_send(1);
    const bool second = small.try_send(2);
    const bool third = small.try_send(3);
    std::printf("try_send 1, 2, 3 into 2 slots: %s %s %s\n", first ? "ok" : "full",
                second ? "ok" : "full", third ? "ok" : "full");
    auto send_three = small.send(3);
    bool sent = false; // this demonstration runs only on this thread
    auto on_sent = [&](const stepwright::error& e) { sent = !e; };
    send_three.start(stepwright::completion::to(on_sent)); // waits: the channel is full
    std::printf("pulled %d\n", small.try_pull().value());  // makes room: the send completes
    std::printf("waiting send completed: %s\n", sent ? "yes" : "failed");
    const int two = small.try_pull().value();
    const int three = small.try_pull().value();
    std::printf("pulled %d, then %d\n", two, three);

    // --- Part 2: the pipeline on a pool -------------------------------------------
    constexpr std::uint64_t per_producer = 500;
    constexpr std::uint64_t producers = 4;
    stepwright::thread_pool pool(4);
    stepwright::channel<std::uint64_t> left(8);
    stepwright::channel<std::uint64_t> right(8);
    stepwright::merge merged(left, right);

    std::uint64_t received = 0; // only the consumer touches these
    std::uint64_t sum = 0;
    std::size_t most_buffered = 0;
    auto consumer = stepwright::for_each(merged, [&](std::uint64_t value) {
        ++received;
        sum += value;
        most_buffered = std::max({most_buffered, left.size(), right.size()});
    });
    consumer.bind(pool);
    stepwright::event consumer_done; // publish the result safely back to main
    auto on_consumed = [&](const stepwright::error& e) { consumer_done.set(e); };
    consumer.start(stepwright::completion::to(on_consumed));

    std::deque<example::producer> machines;
    for (std::uint64_t i = 0; i < producers; ++i) {
        auto& out = i % 2 == 0 ? left : right;
        machines.emplace_back(pool, out, i * per_producer, per_producer);
    }
    stepwright::when_all_range all_producers(machines);
    const stepwright::error produced = stepwright::sync_wait(all_producers);

    left.close(); // the consumer drains what is buffered, then for_each ends
    right.close();
    auto finished = consumer_done.wait();
    const stepwright::error consumed = stepwright::sync_wait(finished);
    pool.wait_idle();

    const std::uint64_t total = producers * per_producer;
    std::printf("received %llu items, sum %llu (expected %llu)\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(sum),
                static_cast<unsigned long long>(total * (total - 1) / 2));
    std::printf("never more than 8 items buffered per channel: %s\n",
                most_buffered <= 8 ? "yes" : "no");
    const bool ok = first && second && !third && !produced && !consumed && received == total &&
                    sum == total * (total - 1) / 2 && most_buffered <= 8;
    return ok ? 0 : 1;
}

// expect: try_send 1, 2, 3 into 2 slots: ok ok full
// expect: pulled 1
// expect: waiting send completed: yes
// expect: pulled 2, then 3
// expect: received 2000 items, sum 1999000 (expected 1999000)
// expect: never more than 8 items buffered per channel: yes
