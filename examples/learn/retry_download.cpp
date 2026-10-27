// A server fails twice, then returns 42 bytes: blocking loop versus retry.
// What you gain:
// - Backoffs release the thread and can be cancelled, unlike sleep_for.
// - Exact exponential-backoff timestamps are tested instantly in virtual time.
// When to use the plain way instead:
// - A short one-off script can reasonably block its only thread.
// - No waits/retries means no benefit; steps add a few ns over direct calls.
#include <stepwright/stepwright.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
int plain_download() {
    auto backoff = 1ms; // tiny real waits keep the example quick; still blocking
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (attempt == 3) {
            return 42;
        }
        std::this_thread::sleep_for(backoff);
        backoff *= 2;
    }
    return 0;
}
struct download : stepwright::machine<download> {
    explicit download(stepwright::timer_service& timers) : clock(timers) { times.reserve(4); }
    stepwright::timer_service& clock;
    std::vector<long long> times;
    int failures = 2;
    int bytes = 0;
    step begin() {
        times.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch())
                .count());
        if (failures-- > 0) {
            return fail(std::errc::io_error, "server unavailable");
        }
        bytes = 42;
        return done();
    }
};
} // namespace

int main() {
    const int expected = plain_download();
    stepwright::testing::virtual_executor clock;
    download file(clock);
    stepwright::retry retried(file, clock, {.max_attempts = 4, .initial_backoff = 1s});
    const auto result = clock.run(retried);
    const bool match = !result && file.bytes == expected && retried.attempts() == 3;
    const bool exact = file.times == std::vector<long long>{0, 1000, 3000};
    std::printf("download results match: %s\n", match ? "yes" : "no");
    for (const auto time : file.times) {
        std::printf("attempt at virtual %lld ms\n", time);
    }

    file.failures = 2;
    file.times.clear();
    stepwright::cancel_source stop;
    stepwright::delay half_second(clock, 500ms);
    auto request =
        stepwright::make_task([&](stepwright::completion done, const stepwright::cancel_token&) {
            stop.request_cancel();
            done({}); // no access to the adapter or captures after this call
        });
    stepwright::sequence cancel_later(half_second, request);
    stepwright::when_all both(retried, cancel_later);
    const auto cancelled = clock.run(both, stop.token());
    const bool stopped = cancelled.is(stepwright::errc::cancelled) && retried.attempts() == 1;
    std::printf("cancelled during backoff, no second attempt: %s\n", stopped ? "yes" : "no");
    return match && exact && stopped ? 0 : 1;
}
// expect: download results match: yes
// expect: attempt at virtual 0 ms
// expect: attempt at virtual 1000 ms
// expect: attempt at virtual 3000 ms
// expect: cancelled during backoff, no second attempt: yes
