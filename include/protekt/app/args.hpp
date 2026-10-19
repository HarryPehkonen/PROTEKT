#pragma once

#include <charconv>
#include <string_view>

namespace protekt::app {

/// Parses a whole command-line argument as T. Strict on purpose: the entire text must be a
/// number, and one that does not fit T is refused. On refusal `value` is left as it was.
template <typename T> bool parse_number(std::string_view text, T& value) {
    T parsed{};
    const char* end = text.data() + text.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): from_chars takes a length.
    const auto [ptr, ec] = std::from_chars(text.data(), end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace protekt::app
