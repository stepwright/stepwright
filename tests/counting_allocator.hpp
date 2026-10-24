#pragma once

// Counting replacement for global allocation, shared by the allocation tests.
// Include from exactly one translation unit per test executable.

#include <atomic>
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
