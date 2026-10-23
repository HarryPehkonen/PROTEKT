#include "protekt/game/roster.hpp"

#include <array>
#include <cstddef>

namespace protekt::game {
namespace {

/// The key a kind does not hold yet. It is one past the digits, so it can never be a real key.
constexpr std::size_t kNoGroup = static_cast<std::size_t>(kRosterKeys);

} // namespace

Roster roster_of(const sim::Sim& sim, sim::Side side) {
    Roster roster;
    std::array<std::size_t, sim::kUnitKindCount> held{};
    held.fill(kNoGroup);
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side != side || !unit.alive) {
            continue;
        }
        const std::size_t kind = static_cast<std::size_t>(unit.kind);
        std::size_t group = held.at(kind);
        if (group == kNoGroup) {
            // The pool's lowest free key is simply the next one: every key below it is held by a
            // kind already met, because this walk hands them out in order and never skips one. A
            // kind whose units are all dead is never met at all, so its key is free again.
            group = roster.groups.size();
            held.at(kind) = group;
            roster.groups.push_back(RosterGroup{static_cast<int>(group) + 1, unit.kind});
        }
        RosterGroup& entry = roster.groups.at(group);
        entry.units.push_back(unit.id);
        entry.hp = static_cast<std::uint16_t>(entry.hp + unit.hp);
        entry.max_hp = static_cast<std::uint16_t>(entry.max_hp + sim::profile(unit.kind).hp);
    }
    return roster;
}

const RosterGroup* group_for_key(const Roster& roster, int key) noexcept {
    for (const RosterGroup& group : roster.groups) {
        if (group.key == key) {
            return &group;
        }
    }
    return nullptr;
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
constexpr std::array<std::string_view, kRosterKeys> kEmpty{
    "You have no group F1.", "You have no group F2.", "You have no group F3.",
    "You have no group F4.", "You have no group F5.", "You have no group F6.",
    "You have no group F7.", "You have no group F8.", "You have no group F9.",
    "You have no group F10."};
} // namespace

std::string_view empty_key_message(int key) noexcept {
    return key >= 1 && key <= kRosterKeys ? kEmpty.at(static_cast<std::size_t>(key - 1))
                                          : std::string_view{};
}

} // namespace protekt::game
