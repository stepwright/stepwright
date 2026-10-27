// Factorial modulo 1,000,000,007: a plain loop versus a cooperative machine.
// The machine yields every 10,000 products; a reporter shares the same thread.
// What you gain:
// - Fairness without creating a thread for each long computation.
// - Deterministic cancellation at a batch boundary, using the start token.
// When to use the plain way instead:
// - Pure numeric work with no waits or other work to schedule.
// - Tight loops: a step costs a few ns, more than a simple loop iteration.
#include <stepwright/stepwright.hpp>

#include <cstdint>
#include <cstdio>

namespace {
constexpr std::uint64_t modulus = 1'000'000'007;
constexpr std::uint64_t limit = 1'000'000;
std::uint64_t plain_factorial(std::uint64_t n) {
    std::uint64_t value = 1;
    for (std::uint64_t i = 1; i <= n; ++i) {
        value = value * i % modulus;
    }
    return value;
}
struct factorial : stepwright::machine<factorial> {
    std::uint64_t value = 1;
    std::uint64_t next = 1;
    bool finished = false;
    step begin() {
        value = next = 1;
        finished = false;
        return then<&factorial::multiply>();
    }
    step multiply() {
        if (cancel_requested()) {
            finished = true;
            return fail(stepwright::errc::cancelled);
        }
        if (next > limit) {
            finished = true;
            return done();
        }
        value = value * next++ % modulus;
        if ((next - 1) % 10'000 == 0) {
            return defer<&factorial::multiply>();
        }
        return then<&factorial::multiply>();
    }
};
struct progress : stepwright::machine<progress> {
    explicit progress(factorial& calculation) : work(calculation) {}
    factorial& work;
    stepwright::cancel_source* stop = nullptr;
    int reports = 0;
    step begin() {
        reports = 0;
        return defer<&progress::report>();
    }
    step report() {
        if (work.finished) {
            return done();
        }
        if (++reports == 3 && stop) {
            stop->request_cancel();
        }
        return defer<&progress::report>();
    }
};
} // namespace

int main() {
    const auto expected = plain_factorial(limit);
    stepwright::testing::virtual_executor clock;
    factorial work;
    progress reporter(work);
    work.bind(clock);
    reporter.bind(clock);
    stepwright::when_all both(work, reporter);
    const auto result = clock.run(both);
    const bool match = !result && work.value == expected;
    const bool shared = reporter.reports > 0;
    stepwright::cancel_source stop;
    reporter.stop = &stop;
    const auto cancelled = clock.run(both, stop.token());
    const bool stopped = cancelled.is(stepwright::errc::cancelled) && work.next <= limit;
    std::printf("factorial results match: %s\n", match ? "yes" : "no");
    std::printf("progress ran between batches: %s\n", shared ? "yes" : "no");
    std::printf("second run cancelled at a deterministic batch: %s\n", stopped ? "yes" : "no");
    return match && shared && stopped ? 0 : 1;
}
// expect: factorial results match: yes
// expect: progress ran between batches: yes
// expect: second run cancelled at a deterministic batch: yes
