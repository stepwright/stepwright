// 04: sharing state with async_mutex, without blocking threads.
//
// A std::mutex held across an asynchronous wait would block a worker thread. An
// async_mutex is a *task*: lock() completes when the caller owns the mutex, and
// waiters queue (FIFO) without occupying any thread. async_mutex::guarded wraps
// the usual pattern in one task: lock, run the inner task, unlock, report the
// inner task's result (it unlocks on failure too).
//
// Below, 16 tellers share one ledger on a 4-thread pool. Each deposit reads the
// balance, then hops to the pool (standing in for an asynchronous write to
// storage) before writing it back. That read-wait-write sequence loses updates
// unless it is exclusive; with the async mutex every deposit counts.
#include "example_support.hpp"
#include <atomic>
#include <cstdio>
#include <deque>
#include <vector>

namespace example {
struct ledger {
    stepwright::async_mutex mutex;
    long balance = 0;             // protected by mutex
    std::atomic<int> inside{0};   // deposits currently in the critical section
    std::atomic<int> overlaps{0}; // times a deposit found another one inside
};

// The critical section, run under the lock by async_mutex::guarded.
struct deposit : stepwright::machine<deposit> {
    ledger* book = nullptr;
    long read = 0;

    step begin() {
        if (book->inside.fetch_add(1) != 0) {
            book->overlaps.fetch_add(1);
        }
        read = book->balance;
        return defer<&deposit::write>(); // asynchronous "storage" round trip
    }
    step write() {
        book->balance = read + 1;
        book->inside.fetch_sub(1);
        return done();
    }
};

// A teller makes `count` deposits, one after the other.
struct teller : stepwright::machine<teller> {
    teller(stepwright::thread_pool& pool, ledger& book, int count)
        : guarded_(book.mutex, deposit_)
        , remaining_(count) {
        deposit_.book = &book;
        deposit_.bind(pool);
        bind(pool);
    }
    step begin() {
        if (remaining_ == 0) {
            return done();
        }
        --remaining_;
        return after<&teller::begin>(guarded_); // lock, deposit, unlock
    }

private:
    deposit deposit_;
    stepwright::async_mutex::guarded guarded_;
    int remaining_;
};
} // namespace example

int main() {
    constexpr int tellers = 16;
    constexpr int deposits_each = 250;
    stepwright::thread_pool pool(4);
    example::ledger book;

    std::deque<example::teller> staff;
    std::vector<stepwright::task*> children;
    children.reserve(tellers);
    for (int i = 0; i < tellers; ++i) {
        children.push_back(&staff.emplace_back(pool, book, deposits_each));
    }
    stepwright::when_all_range everyone(children);
    const stepwright::error e = example::run_blocking(everyone);
    pool.wait_idle();

    std::printf("tellers: %d on %zu threads, %d deposits each\n", tellers, pool.size(),
                deposits_each);
    std::printf("final balance: %ld (expected %d)\n", book.balance, tellers * deposits_each);
    std::printf("overlapping deposits: %d\n", book.overlaps.load());
    constexpr long expected = static_cast<long>(tellers) * deposits_each;
    const bool ok = !e && book.balance == expected && book.overlaps.load() == 0;
    return ok ? 0 : 1;
}

// expect: tellers: 16 on 4 threads, 250 deposits each
// expect: final balance: 4000 (expected 4000)
// expect: overlapping deposits: 0
