#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "protekt/sim/sim.hpp"

/// WHICH WEAPON THE PLAYER HAS UP (M1). Choosing a weapon changes the reticle and the HUD label;
/// the trigger fires the gun's and the rocket's auto-assisted shot as ever, and the MORTAR's shell
/// along the barrel (M2, `sim::orders::FireLob`).
namespace protekt::game {

enum class Weapon : std::uint8_t { Gun, Rocket, Mortar, Bouncer, Shotgun };

/// THE CARRIED SET: what the player's craft carries, in the order the right button walks it. The
/// craft is whatever the player is driving -- the scenario's first player tank, presented as the
/// hover tank -- so the set belongs to the DRIVEN vehicle, not to a sim kind, and an X swap keeps
/// it.
inline constexpr std::array<Weapon, 5> kDrivenWeapons{Weapon::Gun, Weapon::Rocket, Weapon::Mortar,
                                                      Weapon::Bouncer, Weapon::Shotgun};

/// The weapon after `current` in `kDrivenWeapons`, wrapping at the end; one not in the set gives
/// the first. The right button's one step: a weapon change is a click, and the wheel and `Q` that
/// used to do this were removed on the owner's call (2026-10-23).
[[nodiscard]] Weapon next_weapon(Weapon current) noexcept;

/// A VEHICLE'S WEAPON LINEUP (B). Harri's rule: every weapon is available to every vehicle, except
/// that he eventually wants to block some vehicles from some weapons. So the default is the whole
/// carried set, and a row here takes weapons AWAY from one kind. A row cannot add a weapon the game
/// does not carry; it can only express "not this hull". An empty table means every kind carries
/// everything, which is the state today.
///
/// This is configuration only -- the sim still knows nothing about which weapon a hull carries. The
/// trigger sends the order for the weapon that is up (`FireFan` for the shotgun, `FireLob` for the
/// mortar and bouncer, `FireTowards` otherwise), so a lineup decides what a player can SELECT, and
/// the order decides what the sim FIRES. Armory pickups (C) are what will make the sim track it.
struct WeaponBlock {
    sim::UnitKind kind{};
    std::span<const Weapon> denied{};
};

/// No vehicle is blocked from any weapon today. Rows go here as balance asks for them, e.g.
/// `{sim::UnitKind::Scout, kScoutBlocked}` where `kScoutBlocked` names the weapons it may not take.
inline constexpr std::array<WeaponBlock, 0> kWeaponBlocks{};

/// A kind's lineup, in the order the right button walks it: `kDrivenWeapons` without what the kind
/// is blocked from. Returned BY VALUE (five entries at most) so a caller can iterate it without a
/// lifetime trap and without a second table to drift from `kDrivenWeapons`.
struct WeaponLineup {
    std::array<Weapon, kDrivenWeapons.size()> weapons{};
    std::size_t count{};
};

/// May `kind` carry `weapon`? True for every kind and weapon while `kWeaponBlocks` is empty.
/// Defined here rather than in weapon.cpp so every translation unit that asks has the definition
/// (a constexpr function cannot be declared here and defined in one .cpp).
[[nodiscard]] constexpr bool weapon_allowed(sim::UnitKind kind, Weapon weapon) noexcept {
    // A block row names one hull and the weapons it may NOT take. No row today: every kind carries
    // everything. A row cannot add a weapon, only remove one, so `kDrivenWeapons` stays the single
    // definition of what the game carries at all.
    for (const WeaponBlock& block : kWeaponBlocks) {
        if (block.kind != kind) {
            continue;
        }
        for (const Weapon denied : block.denied) {
            if (denied == weapon) {
                return false;
            }
        }
    }
    return true;
}

/// The weapons `kind` may carry, in click order. Never empty: the block table cannot deny every
/// weapon without failing its own check (`static_assert` in weapon.cpp).
[[nodiscard]] constexpr WeaponLineup weapon_lineup(sim::UnitKind kind) noexcept {
    WeaponLineup lineup{};
    for (const Weapon weapon : kDrivenWeapons) {
        if (weapon_allowed(kind, weapon)) {
            lineup.weapons.at(lineup.count) = weapon;
            ++lineup.count;
        }
    }
    return lineup;
}

/// The next weapon THIS KIND may carry, wrapping at the end of its own lineup; a weapon the kind is
/// blocked from (or one it no longer carries) gives the first it may carry. This is the click the
/// player's right button makes. The kind-free overload above stays for callers with no hull in
/// hand.
[[nodiscard]] Weapon next_weapon(sim::UnitKind kind, Weapon current) noexcept;

/// The HUD's name for a weapon: GUN, ROCKET, MORTAR, BOUNCER, SHOTGUN.
[[nodiscard]] std::string_view weapon_name(Weapon weapon) noexcept;

/// THE MORTAR'S RANGE LADDER. Its rungs are pitch bands of the view (`view_pitch`, radians, up is
/// positive), and each rung says how far a mortar shell fired at that pitch lands on level ground.
/// The barrel's climb IS the view's pitch: the shell leaves at the mortar's own `bolt_speed` across
/// the map (as the sim's `launch` lobs it) climbing `tan(pitch)` tiles per tile -- capped at the
/// spec's `arc` -- from `kMuzzleHeight`, and loses the spec's `gravity` every tick.
///
/// M2 widened the view's pitch limit to 1.3 rad, so the ladder runs to the mortar's whole 14 tiles:
/// two rungs more than M1's six (1.15 and 1.28 rad, the last landing at 13.9 tiles).
inline constexpr std::size_t kMortarRungCount = 8;
inline constexpr std::array<float, kMortarRungCount> kMortarRungPitches{0.0F, 0.2F, 0.4F,  0.6F,
                                                                        0.8F, 1.0F, 1.15F, 1.28F};

/// Where a mortar shell fired at `pitch` comes down on level ground, in tiles from the muzzle.
/// Pure, and the sim's own integer flight step for step (`advance_projectiles` over flat ground at
/// height 0): a step that would end in the ground ends where the straight segment meets it. A shell
/// that flies its `max_travel` first fizzles there, and that distance is returned.
[[nodiscard]] float mortar_landing_tiles(float pitch) noexcept;

/// The rung whose pitch is nearest `pitch` (ties to the lower), so every pitch the view can take
/// has exactly one rung lit: below the first band is the first rung, above the last the last.
[[nodiscard]] std::size_t mortar_rung(float pitch) noexcept;

/// Whether the driven unit's gun is still reloading: the reticle reads it. It is the sim's own
/// cooldown, so the reticle is spent for exactly as long as a trigger would be refused -- 45 ticks
/// after a mortar shell, 15 after a tank's bolt. A unit that is not there is not reloading.
[[nodiscard]] bool weapon_reloading(const sim::Sim& sim, sim::UnitId driven) noexcept;

/// Why a trigger the sim declined was declined, for the HUD: "the turret is deploying." for a
/// turret not yet mounted, otherwise "the <weapon> is reloading.".
[[nodiscard]] std::string_view trigger_refusal(Weapon weapon, const sim::Unit& unit) noexcept;

/// The reticle a weapon draws.
///  * FourTicks: E12's four ticks and dot, unchanged -- the gun, and the shotgun's (a scatter
///    gun is aimed the same way; its cone is the shot's, not the sight's).
///  * Heavy: the same family, a centre dot 3.3x as big, ticks 0.4 long instead of 0.65 and nearly
///    twice as thick -- the rocket, one heavy hit.
///  * Ladder: the centre dot alone, with the mortar's range ladder under it -- the mortar's, and
///    the bouncer's too, whose FIRST landing is the same ballistics (the bounce carries it
///    further).
enum class ReticleKind : std::uint8_t { FourTicks, Heavy, Ladder };

struct ReticleShape {
    ReticleKind kind{ReticleKind::FourTicks};
    /// The lit rung, for a Ladder; 0 otherwise.
    std::size_t rung{};
};

/// Which reticle is drawn: a pure function of the weapon and the view's pitch, nothing else.
[[nodiscard]] ReticleShape reticle_for(Weapon weapon, float pitch) noexcept;

} // namespace protekt::game
