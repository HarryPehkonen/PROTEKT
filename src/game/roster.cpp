#include "protekt/game/roster.hpp"

#include <array>
#include <cstddef>

namespace protekt::game {

Roster roster_of(const sim::Sim& sim, sim::Side side) {
    Roster roster;
    int ordinal = 0; // this side's units so far, dead ones included: the position IS the number
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side != side) {
            continue;
        }
        ++ordinal;
        if (!unit.alive) {
            continue;
        }
        if (ordinal > kRosterKeys) {
            ++roster.unkeyed;
            continue;
        }
        roster.slots.push_back(
            RosterSlot{ordinal, unit.id, unit.kind, unit.hp, sim::profile(unit.kind).hp});
    }
    return roster;
}

KeyLookup unit_for_key(const sim::Sim& sim, sim::Side side, int key) {
    if (key < 1 || key > kRosterKeys) {
        return {};
    }
    int ordinal = 0;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side != side || ++ordinal != key) {
            continue;
        }
        return KeyLookup{unit.alive ? KeyState::Alive : KeyState::Dead, unit.id};
    }
    return {};
}

std::string_view kind_label(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::Tank:
        return "TANK";
    case sim::UnitKind::HoverTank:
        return "HOVER";
    case sim::UnitKind::Mortar:
        return "MORTAR";
    case sim::UnitKind::Rocket:
        return "ROCKET";
    case sim::UnitKind::Turret:
        return "TURRET";
    case sim::UnitKind::Factory:
        return "FACTORY";
    }
    return "UNIT";
}

namespace {
constexpr std::array<std::string_view, kRosterKeys> kDead{
    "Unit 1 is destroyed.", "Unit 2 is destroyed.", "Unit 3 is destroyed.",
    "Unit 4 is destroyed.", "Unit 5 is destroyed.", "Unit 6 is destroyed.",
    "Unit 7 is destroyed.", "Unit 8 is destroyed.", "Unit 9 is destroyed."};
constexpr std::array<std::string_view, kRosterKeys> kEmpty{
    "You have no unit 1.", "You have no unit 2.", "You have no unit 3.",
    "You have no unit 4.", "You have no unit 5.", "You have no unit 6.",
    "You have no unit 7.", "You have no unit 8.", "You have no unit 9."};
} // namespace

std::string_view dead_key_message(int key) noexcept {
    return key >= 1 && key <= kRosterKeys ? kDead.at(static_cast<std::size_t>(key - 1))
                                          : std::string_view{};
}
std::string_view empty_key_message(int key) noexcept {
    return key >= 1 && key <= kRosterKeys ? kEmpty.at(static_cast<std::size_t>(key - 1))
                                          : std::string_view{};
}

} // namespace protekt::game
