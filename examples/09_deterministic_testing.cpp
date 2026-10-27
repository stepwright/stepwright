// 09: deterministic testing with testing::virtual_executor.
//
// virtual_executor is an executor *and* a timer service for tests. It runs work
// only inside run_*() calls, on the calling thread; time is virtual (a 5 s delay
// takes no real time); and with schedule::random it picks the next ready item
// with a seeded generator. The same seed always gives the same interleaving, so
// a test can explore many interleavings and replay a failing one exactly.
//
// The bug: two withdrawals each read the balance, hop to the executor (standing
// in for an asynchronous call), and write back the balance minus their amount.
// Some interleavings lose an update. We sweep 200 seeds, replay the first
// failing one with a trace, then show that async_mutex::guarded fixes it.
#include <stepwright/stepwright.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace std::chrono_literals;

namespace example {
struct account {
    int balance = 100;
    std::string trace; // filled only when replaying
};

struct withdraw : stepwright::machine<withdraw> {
    account* acct = nullptr;
    const char* name = "";
    int amount = 0;
    int seen = 0;

    step begin() { return defer<&withdraw::read>(); }
    step read() {
        seen = acct->balance;
        acct->trace += std::string(name) + " reads " + std::to_string(seen) + "; ";
        return defer<&withdraw::write>(); // the race window
    }
    step write() {
        acct->balance = seen - amount;
        acct->trace += std::string(name) + " writes " + std::to_string(acct->balance) + "; ";
        return done();
    }
};

// One run under one seed: returns the final balance (correct: 100 - 30 - 50).
int run_once(std::uint64_t seed, bool locked, account& acct) {
    stepwright::testing::virtual_executor ex(stepwright::testing::schedule::random, seed);
    withdraw a;
    withdraw b;
    a.acct = b.acct = &acct;
    a.name = "A";
    b.name = "B";
    a.amount = 30;
    b.amount = 50;
    a.bind(ex);
    b.bind(ex);
    stepwright::async_mutex mutex;
    stepwright::async_mutex::guarded guarded_a(mutex, a);
    stepwright::async_mutex::guarded guarded_b(mutex, b);
    stepwright::task& first = locked ? static_cast<stepwright::task&>(guarded_a) : a;
    stepwright::task& second = locked ? static_cast<stepwright::task&>(guarded_b) : b;
    stepwright::when_all both(first, second);
    return ex.run(both) ? -1 : acct.balance; // no threads, no real time
}

struct sweep {
    int failures = 0;
    std::int64_t first_failure = -1;
};
sweep explore(int seeds, bool locked) {
    sweep result;
    for (int seed = 0; seed < seeds; ++seed) {
        account acct;
        if (run_once(static_cast<std::uint64_t>(seed), locked, acct) != 20) {
            ++result.failures;
            if (result.first_failure < 0) {
                result.first_failure = seed;
            }
        }
    }
    return result;
}
} // namespace example

int main() {
    // Virtual time: a 5 s delay completes as soon as nothing else is ready.
    stepwright::testing::virtual_executor clock;
    stepwright::delay five_seconds(clock, 5s);
    const auto waited = clock.run(five_seconds);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch());
    std::printf("virtual time after a 5 s delay: %lld ms (no real waiting)\n",
                static_cast<long long>(elapsed.count()));

    constexpr int seeds = 200;
    const example::sweep racy = example::explore(seeds, false);
    std::printf("unlocked: %d of %d seeds lose an update; first failing seed: %lld\n",
                racy.failures, seeds, static_cast<long long>(racy.first_failure));

    // Replay the failing seed: exactly the same interleaving, every time.
    example::account replay;
    const int balance =
        example::run_once(static_cast<std::uint64_t>(racy.first_failure), false, replay);
    std::printf("replay of seed %lld: balance %d, trace: %s\n",
                static_cast<long long>(racy.first_failure), balance, replay.trace.c_str());

    const example::sweep fixed = example::explore(seeds, true);
    std::printf("with async_mutex::guarded: %d of %d seeds fail\n", fixed.failures, seeds);

    const bool ok =
        !waited && elapsed == 5s && racy.failures > 0 && balance != 20 && fixed.failures == 0;
    return ok ? 0 : 1;
}

// expect: virtual time after a 5 s delay: 5000 ms (no real waiting)
// expect: unlocked: 108 of 200 seeds lose an update; first failing seed: 0
// expect: replay of seed 0: balance 50, trace: B reads 100; A reads 100; A writes 70; B writes 50;
// expect: with async_mutex::guarded: 0 of 200 seeds fail
