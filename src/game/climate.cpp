#include "protekt/game/climate.hpp"

#include <algorithm>

#include "protekt/game/scene.hpp"

namespace protekt::game {

namespace {

using math::Vec3;

constexpr std::array<std::string_view, kClimateCount> kNames{"temperate", "sand", "ice",
                                                             "regolith"};

constexpr Vec3 kNoGain{1.0F, 1.0F, 1.0F};

// THE GROUND GAINS. A terrain quad's vertex colour multiplies the atlas tile it samples, so to turn
// grass into sand the vertex has to carry (the colour wanted) / (the tile's mean colour). The
// tiles' means, measured off assets/atlas.png over each family's seven tiles:
//   Flat (grass)  0.254 0.498 0.198     Rough (dirt)  0.439 0.390 0.226
//   Ridge (rock)  0.590 0.551 0.530     Crater        0.193 0.169 0.150
// and the colour each climate wants of each family, so a gain below is wanted / mean:
//   sand      Flat .78 .64 .40   Rough .66 .52 .32   Ridge .72 .62 .48   Crater .34 .26 .17
//   ice       Flat .80 .88 .94   Rough .62 .72 .82   Ridge .70 .78 .86   Crater .22 .30 .40
//   regolith  Flat .40 .39 .38   Rough .33 .32 .31   Ridge .52 .51 .50   Crater .14 .14 .15
// The tile's own pattern -- the speckle, the cracks, the macro tint -- rides on top, because the
// gain is a multiply and not a replacement.
const std::array<ClimateLook, kClimateCount> kLooks{{
    // Temperate: the original look. No sky of its own, no haze, gain 1 exactly.
    {false, {}, {}, 1.0F, {{kNoGain, kNoGain, kNoGain, kNoGain}}},
    // Sand: a dust-hazed amber sky, and a haze that closes in at 70% of the view.
    {true,
     {0.84F, 0.68F, 0.46F},
     {0.56F, 0.50F, 0.44F},
     0.70F,
     {{{3.07F, 1.29F, 2.02F},
       {1.50F, 1.33F, 1.42F},
       {1.22F, 1.13F, 0.91F},
       {1.76F, 1.54F, 1.13F}}}},
    // Ice: a pale, cold sky and a near whiteout at 55% of the view.
    {true,
     {0.86F, 0.91F, 0.96F},
     {0.62F, 0.72F, 0.84F},
     0.55F,
     {{{3.15F, 1.77F, 4.75F},
       {1.41F, 1.85F, 3.63F},
       {1.19F, 1.42F, 1.62F},
       {1.14F, 1.78F, 2.67F}}}},
    // Regolith: airless. A black sky over a dim grey horizon, and nothing to haze -- the fog is
    // total only at the rim of what is meshed, where the ground fades into the dark.
    {true,
     {0.10F, 0.10F, 0.11F},
     {0.005F, 0.005F, 0.015F},
     1.0F,
     {{{1.57F, 0.78F, 1.92F},
       {0.75F, 0.82F, 1.37F},
       {0.88F, 0.93F, 0.94F},
       {0.73F, 0.83F, 1.00F}}}},
}};

} // namespace

std::string_view climate_name(Climate climate) noexcept {
    return kNames.at(static_cast<std::size_t>(climate));
}

std::optional<Climate> climate_from_name(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames.at(i) == name) {
            return static_cast<Climate>(i);
        }
    }
    return std::nullopt;
}

const ClimateLook& climate_look(Climate climate) noexcept {
    return kLooks.at(static_cast<std::size_t>(climate));
}

Vec3 sky_ring_colour(Climate climate, std::size_t ring) noexcept {
    const ClimateLook& look = climate_look(climate);
    if (!look.has_sky) {
        return kSkyRingColours.at(ring);
    }
    // Horizon at and below elevation 0, the zenith at 90, eased so the sky stays near its horizon
    // colour low down, where the eye spends its time.
    const float up = std::clamp(kSkyRingElevationDegrees.at(ring) / 90.0F, 0.0F, 1.0F);
    const float ease = 1.0F - ((1.0F - up) * (1.0F - up));
    return Vec3{look.horizon[0] + ((look.zenith[0] - look.horizon[0]) * ease),
                look.horizon[1] + ((look.zenith[1] - look.horizon[1]) * ease),
                look.horizon[2] + ((look.zenith[2] - look.horizon[2]) * ease)};
}

Vec3 fog_colour(Climate climate) noexcept { return sky_ring_colour(climate, kSkyHorizonRing); }

float fog_far_metres(int radius_tiles, Climate climate) noexcept {
    return fog_far_metres(radius_tiles) * climate_look(climate).fog_far_scale;
}

} // namespace protekt::game
