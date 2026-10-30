#pragma once

#include <cstdint>

#include "protekt/sim/units.hpp"

namespace protekt::game {

/// WHICH PEOPLE a side plays: humans or aliens. Harri's ruling, stated twice: a Scavenger and a
/// Harvester are the SAME machine, differing by FACTION and never by player/enemy side, so the sim
/// keeps ONE kind for the pair, forever. A faction is therefore a presentation and data axis of the
/// game layer -- what a thing is CALLED and what a menu lists -- and the sim does not know the
/// word.
enum class Faction : std::uint8_t {
    Human = 0,
    Alien,
};

/// Which faction each side plays. It belongs to the scenario's presentation (`SceneOptions`), not
/// to a constant: by default the player plays Human and the enemy plays Alien, but a story mission
/// must be able to swap them, and nothing downstream may assume the player is human.
struct Factions {
    Faction player{Faction::Human};
    Faction enemy{Faction::Alien};
};

/// The faction `side` plays under `factions`.
[[nodiscard]] constexpr Faction faction_of(const Factions& factions, sim::Side side) noexcept {
    return side == sim::Side::Player ? factions.player : factions.enemy;
}

} // namespace protekt::game
