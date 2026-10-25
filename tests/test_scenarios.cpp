// Real multi-threaded workloads combining primitives and streams on a thread_pool,
// with every machine bound to the pool.
#include <gtest/gtest.h>

#include "test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
constexpr std::size_t pool_threads = 8;

// Drains a manual_executor on its own thread: a source of completions that arrive
// from outside the pool.
class foreign_thread {
public:
    foreign_thread()
        : thread_([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                if (!executor_.run_one()) {
                    std::this_thread::yield();
                }
            }
            executor_.run_all();
        }) {}
    foreign_thread(const foreign_thread&) = delete;
    foreign_thread& operator=(const foreign_thread&) = delete;
    ~foreign_thread() {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }
    stepwright::executor& executor() noexcept { return executor_; }

private:
    stepwright::manual_executor executor_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Waits for a flag with a generous hang limit; aborts (cannot unwind running work).
void wait_or_abort(const test_support::receiver& r, const char* what) {
    const auto limit = std::chrono::steady_clock::now() + 60s;
    while (!r.completed.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > limit) {
            std::cerr << "scenario hang: " << what << '\n';
            std::abort();
        }
        std::this_thread::yield();
    }
}

// Hops onto the pool before forwarding to a task, so that the task starts on a
// worker (its children's posts then land on that worker's queue and are stolen).
struct launcher : stepwright::machine<launcher> {
    stepwright::task* target = nullptr;
    step begin() { return defer<&launcher::go>(); }
    step go() { return forward(*target); }
};

// --- fan-out / fan-in ------------------------------------------------------------

struct fan_machine : stepwright::machine<fan_machine> {
    fan_machine(stepwright::thread_pool& pool, stepwright::executor& foreign)
        : pool_(&pool)
        , to_pool_(pool)
        , to_foreign_(foreign) {
        bind(pool);
    }
    step begin() {
        steps = 0;
        return after<&fan_machine::first>(to_pool_);
    }
    step first() {
        check();
        return after<&fan_machine::second>(to_foreign_); // completes off the pool
    }
    step second() {
        check();
        return after<&fan_machine::third>(to_pool_);
    }
    step third() {
        check();
        return done();
    }
    void check() noexcept {
        ++steps;
        if (!pool_->running_in_this_thread()) {
            ++off_pool;
        }
    }
    int steps = 0;
    int off_pool = 0;

private:
    stepwright::thread_pool* pool_;
    stepwright::reschedule to_pool_;
    stepwright::reschedule to_foreign_;
};
} // namespace

TEST(scenarios, fan_out_fan_in) {
    constexpr std::size_t count = 10'000;
    const int rounds = test_support::stress_iterations(2);
    foreign_thread foreign;
    stepwright::thread_pool pool(pool_threads);
    std::vector<std::unique_ptr<fan_machine>> machines;
    std::vector<stepwright::task*> children;
    for (std::size_t i = 0; i < count; ++i) {
        machines.push_back(std::make_unique<fan_machine>(pool, foreign.executor()));
        children.push_back(machines.back().get());
    }
    stepwright::when_all_range all(children);
    for (int round = 0; round < rounds; ++round) {
        launcher start;
        start.target = &all;
        start.bind(pool);
        test_support::receiver done;
        start.start(done.callback());
        wait_or_abort(done, "fan_out_fan_in");
        EXPECT_FALSE(done.result) << done.result.describe();
        long steps = 0;
        long off_pool = 0;
        for (const auto& m : machines) {
            steps += m->steps;
            off_pool += m->off_pool;
        }
        EXPECT_EQ(steps, static_cast<long>(3 * count));
        EXPECT_EQ(off_pool, 0);
    }
    pool.wait_idle();
}

// --- pipeline ----------------------------------------------------------------------

namespace {
struct producer : stepwright::machine<producer> {
    stepwright::channel<std::uint64_t>* channel = nullptr;
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    std::uint64_t sent = 0;
    std::optional<stepwright::channel<std::uint64_t>::send_op> send;
    step begin() {
        if (sent == count) {
            return done();
        }
        send.emplace(*channel, first + sent);
        return after<&producer::on_sent>(*send);
    }
    step on_sent() {
        ++sent;
        // Yield to the pool now and then so producers interleave.
        return sent % 64 == 0 ? defer<&producer::begin>() : then<&producer::begin>();
    }
};
} // namespace

TEST(scenarios, pipeline_channels_merge_for_each) {
    constexpr int producers = 8;
    const auto per_producer = static_cast<std::uint64_t>(test_support::stress_iterations(5'000));
    stepwright::thread_pool pool(pool_threads);
    stepwright::channel<std::uint64_t> left(16);
    stepwright::channel<std::uint64_t> right(16);
    stepwright::merge merged(left, right);
    std::uint64_t received = 0;
    std::uint64_t sum = 0;
    std::uint64_t off_pool = 0;
    auto consumer = stepwright::for_each(merged, [&](std::uint64_t value) {
        ++received;
        sum += value;
        off_pool += pool.running_in_this_thread() ? 0U : 1U;
    });
    consumer.bind(pool);
    test_support::receiver consumer_done;
    consumer.start(consumer_done.callback());

    std::array<producer, producers> machines;
    std::array<test_support::receiver, producers> producer_done;
    std::array<launcher, producers> starters;
    for (int i = 0; i < producers; ++i) {
        auto& p = machines[static_cast<std::size_t>(i)];
        p.channel = i % 2 == 0 ? &left : &right;
        p.first = static_cast<std::uint64_t>(i) * per_producer;
        p.count = per_producer;
        p.bind(pool);
        auto& s = starters[static_cast<std::size_t>(i)];
        s.target = &p;
        s.bind(pool);
        s.start(producer_done[static_cast<std::size_t>(i)].callback());
    }
    for (const auto& r : producer_done) {
        wait_or_abort(r, "pipeline producers");
        EXPECT_FALSE(r.result);
    }
    left.close();
    right.close();
    wait_or_abort(consumer_done, "pipeline consumer");
    EXPECT_FALSE(consumer_done.result) << consumer_done.result.describe();
    const std::uint64_t total = producers * per_producer;
    EXPECT_EQ(received, total);
    EXPECT_EQ(sum, total * (total - 1) / 2);
    // Readiness is delivered by producers running on the pool, or by close() from
    // this thread, which posts the bound consumer to the pool.
    EXPECT_EQ(off_pool, 0U);
    pool.wait_idle();
}

// --- contention --------------------------------------------------------------------

namespace {
struct shared_state {
    stepwright::async_mutex mutex;
    std::atomic<int> inside{0};
    std::atomic<bool> violated{false};
    long counter = 0; // protected by mutex only
};

struct critical : stepwright::machine<critical> {
    shared_state* state = nullptr;
    step begin() {
        if (state->inside.fetch_add(1) != 0) {
            state->violated.store(true);
        }
        ++state->counter;
        return defer<&critical::leave>(); // hold the lock across a pool hop
    }
    step leave() {
        state->inside.fetch_sub(1);
        return done();
    }
};

struct contender : stepwright::machine<contender> {
    contender(stepwright::thread_pool& pool, shared_state& state, int iterations)
        : guard_(state.mutex, inner_)
        , remaining_(iterations) {
        inner_.state = &state;
        inner_.bind(pool);
        bind(pool);
    }
    step begin() {
        if (remaining_ == 0) {
            return done();
        }
        --remaining_;
        return after<&contender::begin>(guard_);
    }

private:
    critical inner_;
    stepwright::async_mutex::guarded guard_;
    int remaining_;
};
} // namespace

TEST(scenarios, contention_async_mutex_guarded) {
    constexpr int machines = 64;
    const int iterations = test_support::stress_iterations(500);
    stepwright::thread_pool pool(pool_threads);
    shared_state state;
    std::vector<std::unique_ptr<contender>> contenders;
    std::vector<std::unique_ptr<launcher>> starters;
    std::vector<test_support::receiver> done(machines);
    for (int i = 0; i < machines; ++i) {
        contenders.push_back(std::make_unique<contender>(pool, state, iterations));
        starters.push_back(std::make_unique<launcher>());
        starters.back()->target = contenders.back().get();
        starters.back()->bind(pool);
    }
    for (int i = 0; i < machines; ++i) {
        starters[static_cast<std::size_t>(i)]->start(done[static_cast<std::size_t>(i)].callback());
    }
    for (const auto& r : done) {
        wait_or_abort(r, "contention");
        EXPECT_FALSE(r.result);
    }
    EXPECT_FALSE(state.violated.load());
    EXPECT_EQ(state.counter, static_cast<long>(machines) * iterations);
    pool.wait_idle();
}

// --- timeouts with random cancellation ---------------------------------------------

namespace {
struct outcome_counts {
    std::atomic<long> ok{0};
    std::atomic<long> timed_out{0};
    std::atomic<long> cancelled{0};
    std::atomic<long> other{0};
};

struct sleepy : stepwright::machine<sleepy> {
    sleepy(stepwright::thread_pool& pool, stepwright::timer_service& timers) : timers_(&timers) {
        bind(pool);
    }
    void set(std::chrono::microseconds d) { nap_.emplace(*timers_, d); }
    step begin() {
        if (!nap_) {
            return fail(stepwright::error::make(std::errc::invalid_argument));
        }
        return after<&sleepy::woke>(*nap_);
    }
    step woke() { return defer<&sleepy::finish_up>(); }
    step finish_up() { return done(); }

private:
    stepwright::timer_service* timers_;
    std::optional<stepwright::delay> nap_;
};

struct recorder : stepwright::machine<recorder> {
    recorder(stepwright::thread_pool& pool, stepwright::timer_service& timers,
             outcome_counts& counts)
        : inner_(pool, timers)
        , counts_(&counts) {
        bind(pool);
    }
    void prepare(stepwright::timer_service& timers, std::chrono::microseconds nap,
                 std::chrono::microseconds timeout) {
        inner_.set(nap);
        deadline_.emplace(inner_, timers, timeout);
    }
    step begin() {
        if (!deadline_) {
            return fail(stepwright::error::make(std::errc::invalid_argument));
        }
        return after<&recorder::finished>(*deadline_);
    }
    step finished(const stepwright::error& e) {
        ++completions;
        if (!e) {
            counts_->ok.fetch_add(1);
        } else if (e.is(stepwright::errc::timed_out)) {
            counts_->timed_out.fetch_add(1);
        } else if (e.is(stepwright::errc::cancelled)) {
            counts_->cancelled.fetch_add(1);
        } else {
            counts_->other.fetch_add(1);
        }
        return done(); // keep the when_all result independent of the outcome
    }
    int completions = 0;

private:
    sleepy inner_;
    std::optional<stepwright::with_deadline> deadline_;
    outcome_counts* counts_;
};
} // namespace

TEST(scenarios, timeouts_with_random_cancellation) {
    constexpr std::size_t units = 64;
    const int rounds = test_support::stress_iterations(200);
    stepwright::steady_timer_service timers;
    stepwright::thread_pool pool(pool_threads);
    outcome_counts counts;
    std::vector<std::unique_ptr<recorder>> recorders;
    std::vector<stepwright::task*> children;
    for (std::size_t i = 0; i < units; ++i) {
        recorders.push_back(std::make_unique<recorder>(pool, timers, counts));
        children.push_back(recorders.back().get());
    }
    stepwright::when_all_range all(children);
    std::mt19937 rng(2026);
    std::uniform_int_distribution<int> micros(0, 400);
    std::uniform_int_distribution<int> coin(0, 2);
    long cancelled_rounds = 0;
    for (int round = 0; round < rounds; ++round) {
        for (auto& r : recorders) {
            r->prepare(timers, std::chrono::microseconds(micros(rng)),
                       std::chrono::microseconds(micros(rng)));
        }
        launcher start;
        start.target = &all;
        start.bind(pool);
        stepwright::cancel_source source;
        test_support::receiver done;
        start.start(done.callback(), source.token());
        if (coin(rng) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(micros(rng)));
            source.request_cancel();
            ++cancelled_rounds;
        }
        wait_or_abort(done, "timeouts");
        // when_all reports errc::cancelled when its token was cancelled.
        EXPECT_TRUE(!done.result || done.result.is(stepwright::errc::cancelled))
            << done.result.describe();
    }
    long completions = 0;
    for (const auto& r : recorders) {
        completions += r->completions;
    }
    std::cout << "rounds=" << rounds << " cancelled_rounds=" << cancelled_rounds
              << " ok=" << counts.ok.load() << " timed_out=" << counts.timed_out.load()
              << " cancelled=" << counts.cancelled.load() << '\n';
    EXPECT_EQ(completions, static_cast<long>(units) * rounds);
    EXPECT_EQ(counts.ok.load() + counts.timed_out.load() + counts.cancelled.load(),
              static_cast<long>(units) * rounds);
    EXPECT_EQ(counts.other.load(), 0);
    EXPECT_GT(counts.ok.load(), 0);
    EXPECT_GT(counts.timed_out.load(), 0);
    pool.wait_idle();
}
