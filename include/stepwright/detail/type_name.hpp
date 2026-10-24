#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace stepwright::detail {
template <class T>
constexpr auto type_name_storage() noexcept {
#if defined(__clang__) || defined(__GNUC__)
    constexpr std::string_view signature = __PRETTY_FUNCTION__;
    constexpr auto first = signature.find("T = ") + 4;
    constexpr auto alias = signature.find(';', first);
    constexpr auto last = alias == std::string_view::npos ? signature.rfind(']') : alias;
    constexpr auto name = signature.substr(first, last - first);
#else
    constexpr std::string_view name = "unknown type";
#endif
    std::array<char, name.size() + 1> result{};
    for (std::size_t i = 0; i < name.size(); ++i) {
        result[i] = name[i];
    }
    return result;
}
template <class T>
inline constexpr auto type_name_value = type_name_storage<T>();
template <class T>
/// Borrow a null-terminated compile-time type name with static lifetime. No RTTI
/// or allocation; thread-safe. Unsupported compilers return a descriptive fallback.
constexpr const char* type_name() noexcept {
    return type_name_value<T>.data();
}

template <auto V>
constexpr auto value_name_storage() noexcept {
#if defined(__clang__) || defined(__GNUC__)
    constexpr std::string_view signature = __PRETTY_FUNCTION__;
    constexpr auto first = signature.find("V = ") + 4;
    constexpr auto alias = signature.find(';', first);
    constexpr auto last = alias == std::string_view::npos ? signature.rfind(']') : alias;
    constexpr auto name = signature.substr(first, last - first);
#else
    constexpr std::string_view name = "unknown value";
#endif
    std::array<char, name.size() + 1> result{};
    for (std::size_t i = 0; i < name.size(); ++i) {
        result[i] = name[i];
    }
    return result;
}
template <auto V>
inline constexpr auto value_name_value = value_name_storage<V>();
template <auto V>
/// Borrow the compile-time spelling of a template value argument (for a step,
/// "&my_machine::on_fetched"), with static lifetime. No allocation; thread-safe.
constexpr const char* value_name() noexcept {
    return value_name_value<V>.data();
}
} // namespace stepwright::detail
