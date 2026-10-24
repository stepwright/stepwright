#pragma once

#include <atomic>
#include <thread>

namespace stepwright::detail {
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Small test-and-test-and-set lock for the short, bounded critical sections of
// the primitives (list links, counters, heap updates). Never held while running
// user code, delivering completions or blocking. Spins briefly, then yields.
class spin_lock {
public:
    spin_lock() noexcept = default;
    spin_lock(const spin_lock&) = delete;
    spin_lock& operator=(const spin_lock&) = delete;
    ~spin_lock() = default;

    void lock() noexcept {
        for (;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) {
                return;
            }
            int spins = 0;
            while (locked_.load(std::memory_order_relaxed)) {
                if (++spins < 64) {
                    cpu_relax();
                } else {
                    std::this_thread::yield();
                }
            }
        }
    }
    bool try_lock() noexcept {
        return !locked_.load(std::memory_order_relaxed) &&
               !locked_.exchange(true, std::memory_order_acquire);
    }
    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> locked_{false};
};
} // namespace stepwright::detail
