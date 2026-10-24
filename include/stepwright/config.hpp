#pragma once

#include <cstdio>
#include <cstdlib>

// Keep tiny handle operations visible to the optimizer without requiring an
// extension on unsupported compilers. This affects code generation, never layout.
#if __has_cpp_attribute(gnu::always_inline)
#define STEPWRIGHT_ALWAYS_INLINE [[gnu::always_inline]] inline
#else
#define STEPWRIGHT_ALWAYS_INLINE inline
#endif

// STEPWRIGHT_DEBUG may be set by the consumer before inclusion. Otherwise it follows
// NDEBUG. It controls checks only, never layout; configure consistently per TU.
#ifndef STEPWRIGHT_DEBUG
#ifdef NDEBUG
#define STEPWRIGHT_DEBUG 0
#else
#define STEPWRIGHT_DEBUG 1
#endif
#endif

namespace stepwright::detail {
[[noreturn]] inline void assertion_failed(const char* expr, const char* msg, const char* file,
                                          int line) noexcept {
    std::fprintf(stderr, "stepwright assertion: %s (%s), %s:%d\n", expr, msg, file, line);
    std::abort();
}
} // namespace stepwright::detail

// Debug precondition assertion: print diagnostics and abort, never throw. When
// disabled, neither argument is evaluated; do not depend on assertion side effects.
#if STEPWRIGHT_DEBUG
#define STEPWRIGHT_ASSERT(cond, msg)                                                               \
    ((cond) ? static_cast<void>(0)                                                                 \
            : ::stepwright::detail::assertion_failed(#cond, msg, __FILE__, __LINE__))
#else
#define STEPWRIGHT_ASSERT(cond, msg) static_cast<void>(0)
#endif

// Live task registry (debug/live_tasks.hpp). Set by the CMake option
// STEPWRIGHT_TRACK_TASKS on the `stepwright` interface target. Unlike STEPWRIGHT_DEBUG it
// CHANGES CLASS LAYOUT (machine<D>, source_machine<D, T>, step, source_step), so
// it must have the same value in every translation unit of a program.
#ifndef STEPWRIGHT_TRACK_TASKS
#define STEPWRIGHT_TRACK_TASKS 0
#endif
