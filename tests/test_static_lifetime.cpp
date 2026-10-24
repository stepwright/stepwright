// Regression for the M3 review (spec M4 §0): detail::cancelled_error() used to be
// a function-local static, destroyed at exit. A thread still running during static
// destruction (a detached thread, a timer service owned by another static) then
// copied a destroyed handle whose payload had been freed.
//
// The check runs during static destruction: `late_user` below is constructed
// before main, so it is destroyed after every function-local static first used
// by the test (static objects are destroyed in reverse order of construction).
// Its destructor keeps a thread copying the shared error after main has returned,
// then checks every copy. Freed memory is scribbled by the replacement operator
// delete below, so a use after free is detected deterministically even without
// AddressSanitizer (which reports it on its own).
#include <stepwright/detail/wait_core.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#ifndef STEPWRIGHT_NO_ALLOC_REPLACEMENT
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace {
#if __has_cpp_attribute(gnu::noinline)
[[gnu::noinline]]
#endif
void scribble_and_free(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
#if defined(__APPLE__)
    const std::size_t size = malloc_size(ptr);
#else
    const std::size_t size = malloc_usable_size(ptr);
#endif
    std::memset(ptr, 0xA5, size);
    std::free(ptr);
}
void* allocate(std::size_t size) {
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
        return ptr;
    }
    throw std::bad_alloc();
}
} // namespace

// Ordinary (non-aligned) forms only; the aligned forms keep their default pairing.
void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}
void operator delete(void* ptr) noexcept {
    scribble_and_free(ptr);
}
void operator delete[](void* ptr) noexcept {
    scribble_and_free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept {
    scribble_and_free(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
    scribble_and_free(ptr);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    scribble_and_free(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    scribble_and_free(ptr);
}
#endif

namespace {
std::atomic<bool> stop{false};
std::atomic<long> copies{0};
std::atomic<bool> corrupted{false};

bool intact(const stepwright::error& e) noexcept {
    return e.is(stepwright::errc::cancelled) && e.message().empty() && e.context().empty();
}

void copy_until_stopped() noexcept {
    while (!stop.load(std::memory_order_acquire)) {
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): under test
        const stepwright::error copy = stepwright::detail::cancelled_error();
        if (!intact(copy)) {
            corrupted.store(true);
        }
        copies.fetch_add(1, std::memory_order_release);
    }
}

struct late_user {
    std::thread copier;
    late_user() = default;
    late_user(const late_user&) = delete;
    late_user& operator=(const late_user&) = delete;
    ~late_user() {
        if (copier.joinable()) {
            // main has returned and earlier-constructed statics are destroyed:
            // let the running thread make many more copies, then stop it.
            const long seen = copies.load(std::memory_order_acquire);
            while (copies.load(std::memory_order_acquire) < seen + 10'000) {
                std::this_thread::yield();
            }
            stop.store(true, std::memory_order_release);
            copier.join();
        }
        // And a thread started during static destruction.
        std::thread late([] {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): under test
            const stepwright::error copy = stepwright::detail::cancelled_error();
            if (!intact(copy)) {
                corrupted.store(true);
            }
        });
        late.join();
        if (corrupted.load()) {
            std::fputs("cancelled_error() used after destruction\n", stderr);
            std::_Exit(1);
        }
        std::fputs("cancelled_error() intact during static destruction\n", stderr);
    }
};
late_user user;
} // namespace

TEST(static_lifetime, regression_cancelled_error_survives_static_destruction) {
    // First use happens here, after `user` has been constructed.
    const stepwright::error& shared = stepwright::detail::cancelled_error();
    EXPECT_TRUE(intact(shared));
    EXPECT_EQ(&shared, &stepwright::detail::cancelled_error());
    // Copies share the payload; adding context clones, the shared value is unchanged.
    stepwright::error copy = shared;
    copy.add_context("frame");
    EXPECT_TRUE(intact(shared));
    EXPECT_EQ(copy.context().size(), 1U);
    user.copier = std::thread(copy_until_stopped);
    while (copies.load(std::memory_order_acquire) < 100) {
        std::this_thread::yield();
    }
    // The decisive check runs in ~late_user after main returns.
}
