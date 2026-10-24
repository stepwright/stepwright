#include <gtest/gtest.h>

#include "test_support.hpp"
#include <barrier>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0};
void record() noexcept {
    if (counting.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
}
template <class F>
std::size_t count_allocations(F&& fn) {
    allocations = 0;
    counting = true;
    fn();
    counting = false;
    return allocations;
}
} // namespace

// These replacements are deliberately confined to this test executable.
#ifdef STEPWRIGHT_SANITIZER_ALLOC_HOOK
// Linux compiler-rt TSan/LSan define strong new/delete symbols. Their allocator
// hook measures the same operations without replacing the sanitizer's runtime.
extern "C" void __sanitizer_malloc_hook(const volatile void*, std::size_t) {
    record();
}
#else
namespace {
// Keep replacement delete's malloc/free implementation opaque to GCC's
// mismatched-new-delete diagnostic; the allocation/deallocation pair is valid.
#if __has_cpp_attribute(gnu::noinline)
[[gnu::noinline]]
#endif
void free_storage(void* ptr) noexcept {
    std::free(ptr);
}
} // namespace
void* operator new(std::size_t size) {
    record();
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
        return ptr;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* ptr) noexcept {
    free_storage(ptr);
}
void operator delete[](void* ptr) noexcept {
    free_storage(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept {
    free_storage(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
    free_storage(ptr);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    record();
    const auto align = static_cast<std::size_t>(alignment);
    size = size == 0 ? 1 : size;
    if (size <= std::numeric_limits<std::size_t>::max() - (align - 1)) {
        const auto rounded = ((size + align - 1) / align) * align;
        if (void* ptr = std::aligned_alloc(align, rounded)) {
            return ptr;
        }
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* ptr, std::align_val_t) noexcept {
    free_storage(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept {
    free_storage(ptr);
}
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    free_storage(ptr);
}
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    free_storage(ptr);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    free_storage(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    free_storage(ptr);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    free_storage(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    free_storage(ptr);
}
#endif

TEST(alloc, regression_zero_alloc_then_and_sync_child) {
    struct op_type : stepwright::machine<op_type> {
        test_support::sync_child child;
        int remaining = 0;
        step begin() {
            if (remaining-- == 0) {
                return done();
            }
            return then<&op_type::child_step>();
        }
        step child_step() { return after<&op_type::begin>(child); }
    } op;
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        test_support::receiver receiver;
        op.remaining = test_support::stress_iterations(1000);
        EXPECT_EQ(count_allocations([&] { op.start(receiver.callback(), token); }), 0U);
        EXPECT_EQ(receiver.calls, 1);
    }
}
TEST(alloc, regression_zero_alloc_async_child) {
    struct op_type : stepwright::machine<op_type> {
        test_support::async_child child;
        step begin() { return forward(child); }
    } op;
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        test_support::receiver receiver;
        EXPECT_EQ(count_allocations([&] {
                      op.start(receiver.callback(), token);
                      op.child.finish();
                  }),
                  0U);
        EXPECT_EQ(receiver.calls, 1);
    }
}
TEST(alloc, regression_zero_alloc_park_wake_defer) {
    struct op_type : stepwright::machine<op_type> {
        step begin() { return park<&op_type::resumed>(); }
        step resumed() { return defer<&op_type::finish>(); }
        step finish() { return done(); }
    } op;
    stepwright::manual_executor ex;
    op.bind(ex);
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        for (bool conditional : {false, true}) {
            test_support::receiver receiver;
            bool accepted = true;
            EXPECT_EQ(count_allocations([&] {
                          op.start(receiver.callback(), token);
                          if (conditional) {
                              accepted = op.wake_if_parked();
                          } else {
                              op.wake();
                          }
                          ex.run_all();
                      }),
                      0U);
            EXPECT_TRUE(accepted);
            EXPECT_EQ(receiver.calls, 1);
        }
    }
}
TEST(alloc, single_error_payload_and_intrusive_registration) {
    stepwright::error e;
    EXPECT_EQ(count_allocations([&] {
                  e = stepwright::error::make(stepwright::errc::closed,
                                              "inline message longer than SSO capacity");
              }),
              1U);
    EXPECT_EQ(count_allocations([&] {
                  auto copy = e;
                  copy = {};
                  e.add_context("unique");
              }),
              0U);
    stepwright::cancel_source source;
    EXPECT_EQ(
        count_allocations([&] { stepwright::cancel_callback callback(source.token(), [] {}); }),
        0U);
}
TEST(alloc, regression_zero_alloc_foreign_thread_child) {
    struct op_type : stepwright::machine<op_type> {
        test_support::async_child child;
        step begin() { return after<&op_type::finish>(child); }
        step finish() { return done(); }
    } op;
    std::barrier rendezvous(2);
    std::thread worker([&] {
        rendezvous.arrive_and_wait(); // Finish pthread/sanitizer startup before counting.
        for (int i = 0; i < 2; ++i) {
            rendezvous.arrive_and_wait();
            op.child.finish();
            rendezvous.arrive_and_wait();
        }
    });
    rendezvous.arrive_and_wait();
    stepwright::cancel_source source;
    for (const auto& token : {stepwright::cancel_token{}, source.token()}) {
        test_support::receiver receiver;
        EXPECT_EQ(count_allocations([&] {
                      op.start(receiver.callback(), token);
                      rendezvous.arrive_and_wait();
                      receiver.wait();
                      rendezvous.arrive_and_wait();
                  }),
                  0U);
        EXPECT_EQ(receiver.calls, 1);
    }
    worker.join();
}
