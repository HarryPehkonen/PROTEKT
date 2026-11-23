#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "protekt/math/math.hpp"

namespace protekt::game {

/// A MAP'S CLIMATE: what its ground and air LOOK like. It is read from the map's header
/// (`climate = "sand"` under `[map]`) and from nowhere else, so a hazy place and a clear one differ
/// because the map says so.
///
/// THIS IS A LOOK AND ONLY A LOOK. Harri, on sand and ice: "Let's leave the slipperiness alone for
/// now.  I haven't noticed any difference in BZ2."  The missing slipperiness was never wanted, so
/// no climate changes how anything MOVES, and the sim never reads this type (a test pins both
/// halves shut). A climate that wanted a movement rule would be a proposal, not a change.
enum class Climate : std::uint8_t { Temperate, Sand, Ice, Regolith };
inline constexpr std::size_t kClimateCount = 4;

/// The name a map file spells it with: "temperate", "sand", "ice", "regolith".
[[nodiscard]] std::string_view climate_name(Climate climate) noexcept;

/// The climate a name spells, or nothing. STRICT: no case folding, no aliases, no fallback -- "a
/// bad data file must never quietly play a different game".
[[nodiscard]] std::optional<Climate> climate_from_name(std::string_view name) noexcept;

/// Every value a climate sets, in one place.
struct ClimateLook {
    /// The sky at the horizon and straight up. Absent for Temperate, whose sky is the original
    /// ring table (`kSkyRingColours`) exactly. The FOG colour is the horizon ring, so the ground
    /// always dissolves into the sky it stands under.
    bool has_sky{false};
    math::Vec3 horizon{};
    math::Vec3 zenith{};
    /// The distance, as a fraction of the meshed radius, at which the fog is total: 1 is the
    /// original (fog and cull agree), less is haze. Never more than 1, or the rim would show.
    float fog_far_scale{1.0F};
    /// A gain on the ground's vertex colour, one per terrain family (Flat, Rough, Ridge, Crater),
    /// that turns the atlas's tiles toward this climate's ground. Temperate's is 1 exactly.
    std::array<math::Vec3, 4> ground_gain{};
};

[[nodiscard]] const ClimateLook& climate_look(Climate climate) noexcept;

/// Sky ring `ring` (see `kSkyRingColours`) under `climate`.
[[nodiscard]] math::Vec3 sky_ring_colour(Climate climate, std::size_t ring) noexcept;

/// The fog's colour under `climate`: its sky's horizon ring, one source.
[[nodiscard]] math::Vec3 fog_colour(Climate climate) noexcept;

} // namespace protekt::game
