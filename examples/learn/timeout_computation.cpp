// A computation with a budget: hand-polled stop flag versus with_deadline.
// What you gain:
// - Deadlines propagate cancellation through computation and any child waits.
// - A fairness defer lets other work share the executor; tests need no real time.
// When to use the plain way instead:
// - A single synchronous loop with a known budget needs little infrastructure.
// - Steps cost a few ns; do not use them to accelerate numeric loops.
// Virtual time does NOT measure CPU time. Both versions model each batch as 1 ms;
// the machine uses a delay to charge that cost. Real code uses a steady timer and
// checks its token between bounded batches (std::sort cannot be preempted either).
#include <stepwright/stepwright.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>

using namespace std::chrono_literals;
namespace {
struct outcome {
    std::uint64_t sum = 0;
    bool finished = false;
};
outcome plain_compute(int batches, int budget) {
    outcome result;
    bool stop = false; // application must arrange a clock/flag check itself
    for (int batch = 0; batch < batches; ++batch) {
        if (stop) {
            return result;
        }
        for (std::uint64_t i = 1; i <= 1000; ++i) {
            result.sum += i;
        }
        stop = batch + 1 >= budget; // deterministic stand-in for an elapsed clock
    }
    result.finished = true;
    return result;
}
struct computation : stepwright::machine<computation> {
    explicit computation(stepwright::timer_service& timers) : cost(timers, 1ms) {}
    stepwright::delay cost;
    int remaining = 0;
    std::uint64_t sum = 0;
    step begin() {
        remaining = 20;
        sum = 0;
        return defer<&computation::batch>();
    }
    step batch() {
        if (cancel_requested()) {
            return fail(stepwright::errc::cancelled);
        }
        for (std::uint64_t i = 1; i <= 1000; ++i) {
            sum += i;
        }
        --remaining;
        return after<&computation::yield>(cost);
    }
    step yield() {
        if (remaining == 0) {
            return done();
        }
        return defer<&computation::batch>();
    }
};
} // namespace

int main() {
    stepwright::testing::virtual_executor clock;
    computation work(clock);
    work.bind(clock);
    stepwright::with_deadline limited(work, clock, 50ms);
    const auto success = clock.run(limited);
    const auto plain = plain_compute(20, 50);
    const bool match = !success && plain.finished && work.sum == plain.sum;
    const auto start = clock.now();
    const auto timeout = clock.run(limited.with(5ms));
    const auto short_plain = plain_compute(20, 5);
    const bool bounded = timeout.is(stepwright::errc::timed_out) && !short_plain.finished &&
                         work.remaining > 0 && clock.now() - start == 5ms;
    std::printf("computation results match: %s\n", match ? "yes" : "no");
    std::printf("short budget stops both versions: %s\n", bounded ? "yes" : "no");
    std::puts("timeout at exactly 5 virtual ms; no blocked worker");
    return match && bounded ? 0 : 1;
}
// expect: computation results match: yes
// expect: short budget stops both versions: yes
// expect: timeout at exactly 5 virtual ms; no blocked worker
