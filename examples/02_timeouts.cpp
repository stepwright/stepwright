// 02: timeouts with delay and with_deadline.
//
// A steady_timer_service owns one background thread that fires timers. `delay`
// is a task that completes after a duration; `with_deadline` runs another task
// with a time limit and completes with whichever comes first:
//
//   - the inner task: its result (the timer is cancelled);
//   - the timer: errc::timed_out, after cancelling the inner task and waiting
//     for it to finish (structured: nothing is left running behind your back).
//
// The machines are bound to a thread_pool: timer completions arrive on the
// timer thread, and binding moves the next step back onto the pool, so the timer
// thread only ever does a few nanoseconds of work per timer.
#include "example_support.hpp"
#include <chrono>
#include <cstdio>

using namespace std::chrono_literals;

namespace example {
// Pretend to query a backend that answers after `latency`.
struct query : stepwright::machine<query> {
    query(stepwright::timer_service& timers, stepwright::thread_pool& pool,
          stepwright::timer_service::clock::duration latency)
        : wait_(timers, latency) {
        bind(pool);
    }
    bool answered = false;

    step begin() {
        answered = false;
        return after<&query::on_reply>(wait_); // cancellable: wait_ gets our token
    }
    step on_reply() { // no error argument: a failed wait skips this step
        answered = true;
        return done();
    }

private:
    stepwright::delay wait_;
};

void report(const char* what, const stepwright::error& e, const query& q) {
    if (!e) {
        std::printf("%s: finished in time (answered=%s)\n", what, q.answered ? "yes" : "no");
    } else if (e.is(stepwright::errc::timed_out)) {
        std::printf("%s: timed out (answered=%s)\n", what, q.answered ? "yes" : "no");
    } else {
        std::printf("%s: failed: %s\n", what, e.describe().c_str());
    }
}
} // namespace example

int main() {
    stepwright::steady_timer_service timers;
    stepwright::thread_pool pool(2);

    // A plain delay: just wait (the margins below are wide on purpose, so the
    // outcome never depends on how busy the machine is).
    stepwright::delay pause(timers, 5ms);
    const stepwright::error paused = example::run_blocking(pause);
    std::printf("delay of 5 ms: %s\n", paused ? "failed" : "done");

    // A fast backend (1 ms) under a generous limit (5 s): the reply wins.
    example::query fast(timers, pool, 1ms);
    stepwright::with_deadline fast_limited(fast, timers, 5s);
    const stepwright::error fast_result = example::run_blocking(fast_limited);
    example::report("fast query, 5 s limit", fast_result, fast);

    // A slow backend (one minute) under a 20 ms limit: the timer wins, the query
    // is cancelled (its delay completes with errc::cancelled), and only then does
    // with_deadline complete. The program does not wait a minute.
    example::query slow(timers, pool, 60s);
    stepwright::with_deadline slow_limited(slow, timers, 20ms);
    const auto started = std::chrono::steady_clock::now();
    const stepwright::error slow_result = example::run_blocking(slow_limited);
    const bool prompt = std::chrono::steady_clock::now() - started < 30s;
    example::report("slow query, 20 ms limit", slow_result, slow);
    std::printf("slow query returned promptly: %s\n", prompt ? "yes" : "no");

    pool.wait_idle();
    const bool ok =
        !paused && !fast_result && slow_result.is(stepwright::errc::timed_out) && prompt;
    return ok ? 0 : 1;
}

// expect: delay of 5 ms: done
// expect: fast query, 5 s limit: finished in time (answered=yes)
// expect: slow query, 20 ms limit: timed out (answered=no)
// expect: slow query returned promptly: yes
