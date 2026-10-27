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

Weapon next_weapon(sim::UnitKind kind, Weapon current) noexcept {
    const WeaponLineup lineup = weapon_lineup(kind);
    if (lineup.count == 0) {
        return kDrivenWeapons.front(); // unreachable while the static_assert below holds
    }
    for (std::size_t i = 0; i < lineup.count; ++i) {
        if (lineup.weapons.at(i) == current) {
            return lineup.weapons.at((i + 1) % lineup.count);
        }
    }
    // A weapon this kind does not carry (a stale selection after a swap) gives the first it does.
    return lineup.weapons.front();
}

std::string_view weapon_name(Weapon weapon) noexcept {
    switch (weapon) {
    case Weapon::Gun:
        return "GUN";
    case Weapon::Rocket:
        return "ROCKET";
    case Weapon::Mortar:
        return "MORTAR";
    case Weapon::Bouncer:
        return "BOUNCER";
    case Weapon::Shotgun:
        return "SHOTGUN";
    }
    return "GUN";
}

float mortar_landing_tiles(float pitch) noexcept {
    const sim::UnitSpec& mortar = sim::spec_of(sim::UnitKind::Mortar);
    const std::int64_t across = mortar.bolt_speed.raw;
    // The climb in height units a tick, from the sim's own `mortar_climb` (the very call the
    // player's shell is launched with): one tile of run, `tan(pitch)` tiles of rise in Q16.16.
    std::int64_t vz
        = sim::mortar_climb(sim::Fixed::kOne, std::lround(static_cast<double>(std::tan(pitch))
                                                          * static_cast<double>(sim::Fixed::kOne)));
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

bool weapon_reloading(const sim::Sim& sim, sim::UnitId driven) noexcept {
    const sim::Unit* unit = sim.find(driven);
    return unit != nullptr && unit->alive && unit->cooldown > 0;
}

std::string_view trigger_refusal(Weapon weapon, const sim::Unit& unit) noexcept {
    if (!sim::is_mounted(unit)) {
        return "the turret is deploying.";
    }
    switch (weapon) {
    case Weapon::Gun:
        return "the gun is reloading.";
    case Weapon::Rocket:
        return "the rocket is reloading.";
    case Weapon::Mortar:
        return "the mortar is reloading.";
    case Weapon::Bouncer:
        return "the bouncer is reloading.";
    case Weapon::Shotgun:
        return "the shotgun is reloading.";
    }
    return "the weapon is reloading.";
}

ReticleShape reticle_for(Weapon weapon, float pitch) noexcept {
    switch (weapon) {
    case Weapon::Gun:
    case Weapon::Shotgun:
        return ReticleShape{ReticleKind::FourTicks, 0};
    case Weapon::Rocket:
        return ReticleShape{ReticleKind::Heavy, 0};
    case Weapon::Mortar:
    case Weapon::Bouncer:
        return ReticleShape{ReticleKind::Ladder, mortar_rung(pitch)};
    }
    return ReticleShape{};
}

// THE LINEUP CAN NEVER BE EMPTY. A block row that denied every weapon would leave a drivable hull
// with nothing to fire, and the right button with nothing to walk. The table is checked at compile
// time, so the first row that does it fails the build rather than the game.
static_assert(
    [] {
        for (int kind = 0; kind < sim::kUnitKindCount; ++kind) {
            if (weapon_lineup(static_cast<sim::UnitKind>(kind)).count == 0) {
                return false;
            }
        }
        return true;
    }(),
    "every unit kind must keep at least one carried weapon");

} // namespace protekt::game
