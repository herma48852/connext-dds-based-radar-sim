#pragma once

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace radar::cli {

template <typename T>
bool parse_integer(const char* text, T minimum, T maximum, T& value) {
    static_assert(std::is_integral_v<T>);
    if (text == nullptr)
        return false;
    const std::string_view input{text};
    if (input.empty())
        return false;

    T parsed{};
    const auto result = std::from_chars(
        input.data(), input.data() + input.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != input.data() + input.size() ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool parse_finite_double(const char* text, double minimum,
                                double maximum, double& value) {
    if (text == nullptr || *text == '\0')
        return false;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' ||
        !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace radar::cli
