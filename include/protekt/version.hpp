#pragma once

#include <string_view>

namespace protekt {

/// The build's version, generated from project() VERSION in CMakeLists.txt — the only place
/// the number is written. Never hard-code a version anywhere else.
[[nodiscard]] std::string_view version() noexcept;

} // namespace protekt
