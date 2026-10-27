// Fibonacci: build a vector up front, or produce a lazy, unbounded pull stream.
// Both consumers stop at the first number exceeding 1,000,000.
// What you gain:
// - The consumer sets the pace; the source stores only its next two numbers.
// - A producer can wait between values without blocking a thread.
// When to use the plain way instead:
// - You need a small, known finite vector immediately.
// - Numeric loops are faster than steps (a few ns each); this is not a speedup.
#include <stepwright/stepwright.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
constexpr std::uint64_t limit = 1'000'000;
std::vector<std::uint64_t> plain_fibonacci() {
    std::vector<std::uint64_t> values;
    std::uint64_t a = 0;
    std::uint64_t b = 1;
    do {
        values.push_back(a);
        const auto next = a + b;
        a = b;
        b = next;
    } while (values.back() <= limit);
    return values;
}
template <bool Paced>
struct fibonacci : stepwright::source_machine<fibonacci<Paced>, std::uint64_t> {
    using base = stepwright::source_machine<fibonacci<Paced>, std::uint64_t>;
    using typename base::step;
    explicit fibonacci(stepwright::timer_service& clock) : pause(clock, std::chrono::seconds(1)) {}
    stepwright::delay pause;
    std::uint64_t a = 0, b = 1;
    step begin() {
        if constexpr (Paced) {
            return this->template after<&fibonacci::produce>(pause);
        }
        return this->template then<&fibonacci::produce>();
    }
    step produce() {
        const auto value = a;
        a = b;
        b += value;
        return this->template emit<&fibonacci::begin>(value);
    }
};
} // namespace

int main() {
    const auto expected = plain_fibonacci();
    stepwright::testing::virtual_executor clock;
    fibonacci<false> lazy(clock);
    std::vector<std::uint64_t> values;
    auto take = stepwright::for_each(lazy, [&](std::uint64_t n) {
        values.push_back(n);
        return n <= limit; // false stops; the source is between pulls, not suspended
    });
    const auto result = clock.run(take);
    fibonacci<true> paced(clock);
    std::vector<std::uint64_t> paced_values;
    auto take_paced = stepwright::for_each(paced, [&](std::uint64_t n) {
        paced_values.push_back(n);
        return n <= limit;
    });
    const auto paced_result = clock.run(take_paced);
    const bool match = !result && !paced_result && values == expected && paced_values == expected;
    const bool timed = clock.now().time_since_epoch() ==
                       std::chrono::seconds(static_cast<long long>(expected.size()));
    std::printf("Fibonacci results match: %s\n", match ? "yes" : "no");
    std::printf("lazy consumer stops at %llu\n", static_cast<unsigned long long>(values.back()));
    std::printf("paced stream waits once per item in virtual time: %s\n", timed ? "yes" : "no");
    return match && timed ? 0 : 1;
}
// expect: Fibonacci results match: yes
// expect: lazy consumer stops at 1346269
// expect: paced stream waits once per item in virtual time: yes
