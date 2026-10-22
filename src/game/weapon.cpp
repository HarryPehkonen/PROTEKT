#include "protekt/game/weapon.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::game {

Weapon next_weapon(Weapon current) noexcept {
    for (std::size_t i = 0; i < kDrivenWeapons.size(); ++i) {
        if (kDrivenWeapons.at(i) == current) {
            return kDrivenWeapons.at((i + 1) % kDrivenWeapons.size());
        }
    }
    return kDrivenWeapons.front();
}

std::string_view weapon_name(Weapon weapon) noexcept {
    switch (weapon) {
    case Weapon::Gun:
        return "GUN";
    case Weapon::Rocket:
        return "ROCKET";
    case Weapon::Mortar:
        return "MORTAR";
    }
    return "GUN";
}

float mortar_landing_tiles(float pitch) noexcept {
    const sim::UnitSpec& mortar = sim::spec_of(sim::UnitKind::Mortar);
    const std::int64_t across = mortar.bolt_speed.raw;
    const std::int64_t steepest = static_cast<std::int64_t>(mortar.arc.raw) * across
                                  / sim::Fixed::kOne * sim::kHeightUnitsPerTile;
    // The climb in height units a tick: tan(pitch) tiles per tile of run, 64 units to the tile.
    std::int64_t vz = static_cast<std::int32_t>(
        std::tan(pitch) * static_cast<float>(across * sim::kHeightUnitsPerTile));
    vz = std::clamp(vz, -steepest, steepest);
    std::int64_t z = sim::kMuzzleHeight.raw; // over flat ground at height 0
    std::int64_t flown = 0;
    // `advance_projectiles`, step for step: move by the velocity; a step that ends at or under the
    // ground ends where the segment meets it; otherwise check the travel, then lose `gravity`.
    for (std::uint32_t age = 1; age < 10000; ++age) {
        const std::int64_t end = z + vz;
        if (end <= 0) {
            const std::int64_t until = std::max<std::int64_t>(z, 0);
            const std::int64_t whole = until - end;
            flown += whole > 0 ? across * until / whole : 0;
            break;
        }
        flown += across;
        if (static_cast<std::int64_t>(age) * across >= mortar.max_travel.raw) {
            break; // fizzles in the air, as far as it was allowed to fly
        }
        z = end;
        vz -= mortar.gravity.raw;
    }
    return static_cast<float>(flown) / static_cast<float>(sim::Fixed::kOne);
}

std::size_t mortar_rung(float pitch) noexcept {
    std::size_t best = 0;
    for (std::size_t rung = 1; rung < kMortarRungCount; ++rung) {
        if (std::abs(kMortarRungPitches.at(rung) - pitch)
            < std::abs(kMortarRungPitches.at(best) - pitch)) {
            best = rung;
        }
    }
    return best;
}

ReticleShape reticle_for(Weapon weapon, float pitch) noexcept {
    switch (weapon) {
    case Weapon::Gun:
        return ReticleShape{ReticleKind::FourTicks, 0};
    case Weapon::Rocket:
        return ReticleShape{ReticleKind::Heavy, 0};
    case Weapon::Mortar:
        return ReticleShape{ReticleKind::Ladder, mortar_rung(pitch)};
    }
    return ReticleShape{};
}

} // namespace protekt::game
