#pragma once

#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace test_support {
// Shared by all stress tests. Invalid/overflowing scales fail rather than silently
// skipping work. The environment must be set before starting the test executable.
inline int stress_iterations(int base) {
    static const int scale = [] {
        const char* text = std::getenv("STEPWRIGHT_STRESS_SCALE");
        if (!text) {
            return 1;
        }
        const std::string_view input(text);
        int value = 0;
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || value <= 0) {
            throw std::invalid_argument("STEPWRIGHT_STRESS_SCALE must be a positive integer");
        }
        return value;
    }();
    if (base > std::numeric_limits<int>::max() / scale) {
        throw std::out_of_range("STEPWRIGHT_STRESS_SCALE iteration count overflow");
    }
    return base * scale;
}
} // namespace test_support
