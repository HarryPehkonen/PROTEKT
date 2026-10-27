#include "protekt/game/row.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace protekt::game {
namespace {

/// The building key a kind sits on, if it is one: `1` the deployed recycler, `2` the factory. Every
/// other kind is a vehicle, and vehicles sit only on F1..F10.
[[nodiscard]] std::optional<std::size_t> building_slot(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::DeployedRecycler:
        return 1;
    case sim::UnitKind::Factory:
        return 2;
    case sim::UnitKind::Tank:
    case sim::UnitKind::Turret:
    case sim::UnitKind::HoverTank:
    case sim::UnitKind::Mortar:
    case sim::UnitKind::Rocket:
    case sim::UnitKind::Scavenger:
    case sim::UnitKind::Recycler:
    case sim::UnitKind::Scout:
        break;
    }
    return std::nullopt;
}

/// Whether any slot already holds `id`.
bool is_held(const std::array<RowSlot, kRowSlots>& slots, sim::UnitId id) {
    return std::any_of(slots.begin(), slots.end(), [id](const RowSlot& slot) {
        return std::find(slot.units.begin(), slot.units.end(), id) != slot.units.end();
    });
}

/// Whether a GROUP slot (F1..F10) holds `id`: the building keys are refilled from the sim every
/// frame and are not the player's to rearrange.
bool is_held_in_group(const std::array<RowSlot, kRowSlots>& slots, sim::UnitId id) {
    return std::any_of(slots.begin() + kRowFirstGroup, slots.end(), [id](const RowSlot& slot) {
        return std::find(slot.units.begin(), slot.units.end(), id) != slot.units.end();
    });
}

} // namespace

void RowState::update(const sim::Sim& sim, sim::Side side, std::size_t marks) {
    nav_marks = marks;
    // A group key loses its dead, and a vehicle that has become a building (a recycler that has
    // deployed): its F key goes blank where it stands and is never re-packed.
    for (RowSlot& slot : slots) {
        std::erase_if(slot.units, [&sim](sim::UnitId id) {
            const sim::Unit* unit = sim.find(id);
            return unit == nullptr || !unit->alive || building_slot(unit->kind).has_value();
        });
    }
    // The building keys are not remembered: they hold whatever of the thing is alive.
    for (std::size_t index = 0; index < kRowFirstGroup; ++index) {
        slots.at(index).units.clear();
    }
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side != side || !unit.alive) {
            continue;
        }
        if (const std::optional<std::size_t> key = building_slot(unit.kind); key.has_value()) {
            slots.at(*key).units.push_back(unit.id);
            continue;
        }
        // Units come in id order, so a slot's occupants stay in spawn order. A unit that finds
        // every group slot full is simply unseen: it is offered the next slot to free up.
        if (is_held(slots, unit.id)) {
            continue;
        }
        const auto free_slot = std::find_if(slots.begin() + kRowFirstGroup, slots.end(),
                                            [](const RowSlot& slot) { return slot.units.empty(); });
        if (free_slot != slots.end()) {
            free_slot->units.push_back(unit.id);
        }
    }
}

bool RowState::occupied(std::size_t index) const {
    return index == 0 ? nav_marks > 0 : !slots.at(index).units.empty();
}

RowMove RowState::move_into(const sim::Sim& sim, std::span<const sim::UnitId> units,
                            int group_key) {
    if (group_key < 1 || group_key > kRowGroups) {
        return RowMove::NoSuchKey;
    }
    const std::size_t target = group_slot(group_key);

    // Who moves: the living, that a group slot holds, once each.
    std::vector<sim::UnitId> moving;
    for (const sim::UnitId id : units) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->alive && is_held_in_group(slots, id)
            && std::find(moving.begin(), moving.end(), id) == moving.end()) {
            moving.push_back(id);
        }
    }
    if (moving.empty()) {
        return RowMove::NothingToMove;
    }
    const sim::UnitKind kind = sim.find(moving.front())->kind;
    const bool one_kind = std::all_of(moving.begin(), moving.end(), [&sim, kind](sim::UnitId id) {
        return sim.find(id)->kind == kind;
    });
    if (!one_kind) {
        return RowMove::MixedKinds;
    }
    const std::optional<SlotReading> held = read_slot(sim, slots.at(target));
    if (held.has_value() && held->kind != kind) {
        return RowMove::DifferentKind;
    }

    for (std::size_t index = kRowFirstGroup; index < kRowSlots; ++index) {
        if (index == target) {
            continue;
        }
        std::erase_if(slots.at(index).units, [&moving](sim::UnitId id) {
            return std::find(moving.begin(), moving.end(), id) != moving.end();
        });
    }
    std::vector<sim::UnitId>& into = slots.at(target).units;
    for (const sim::UnitId id : moving) {
        if (std::find(into.begin(), into.end(), id) == into.end()) {
            into.push_back(id);
        }
    }
    return RowMove::Moved;
}

std::string_view move_refusal_message(RowMove outcome) noexcept {
    switch (outcome) {
    case RowMove::NothingToMove:
        return "Select the units to move first.";
    case RowMove::MixedKinds:
        return "A key holds one kind; select one kind to move.";
    case RowMove::DifferentKind:
        return "That key holds a different kind.";
    case RowMove::Moved:
    case RowMove::NoSuchKey:
        break;
    }
    return {};
}

std::optional<SlotReading> read_slot(const sim::Sim& sim, const RowSlot& slot) {
    std::optional<SlotReading> reading;
    for (const sim::UnitId id : slot.units) {
        const sim::Unit* unit = sim.find(id);
        if (unit == nullptr || !unit->alive) {
            continue;
        }
        if (!reading.has_value()) {
            reading = SlotReading{unit->kind, 0, 0, 0, {}};
        }
        const std::uint16_t ceiling = sim::profile(unit->kind).hp;
        ++reading->count;
        reading->hp = static_cast<std::uint16_t>(reading->hp + unit->hp);
        reading->max_hp = static_cast<std::uint16_t>(reading->max_hp + ceiling);
        reading->fractions.push_back(
            ceiling == 0 ? 0.0F : static_cast<float>(unit->hp) / static_cast<float>(ceiling));
    }
    return reading;
}

std::vector<BarSegment> bar_segments(std::span<const float> fractions) {
    // The ramp's anchors, lowest first: near-black, red, yellow, bright green.
    constexpr std::array<float, 4> kAt{0.0F, 0.33F, 0.66F, 1.0F};
    constexpr std::array<std::array<float, 3>, 4> kRgb{{{0.12F, 0.05F, 0.05F},
                                                        {1.00F, 0.15F, 0.10F},
                                                        {1.00F, 0.85F, 0.10F},
                                                        {0.15F, 0.95F, 0.25F}}};
    std::vector<BarSegment> segments;
    segments.reserve(fractions.size());
    for (const float raw : fractions) {
        const float fraction = std::clamp(raw, 0.0F, 1.0F);
        std::size_t upper = 1;
        while (upper + 1 < kAt.size() && fraction > kAt.at(upper)) {
            ++upper;
        }
        const float t = (fraction - kAt.at(upper - 1)) / (kAt.at(upper) - kAt.at(upper - 1));
        BarSegment segment{fraction, {}};
        for (std::size_t c = 0; c < 3; ++c) {
            // std::lerp is exact at both ends, so an anchor reads as exactly its own colour.
            segment.rgb.at(c) = std::lerp(kRgb.at(upper - 1).at(c), kRgb.at(upper).at(c), t);
        }
        segments.push_back(segment);
    }
    return segments;
}

std::string_view row_key_label(std::size_t index) noexcept {
    static constexpr std::array<std::string_view, kRowSlots> kLabels{
        "~", "1", "2", "3", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"};
    return index < kRowSlots ? kLabels.at(index) : std::string_view{};
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
    case sim::UnitKind::Scavenger:
        return "SCAVENGER";
    case sim::UnitKind::Scout:
        return "SCOUT";
    case sim::UnitKind::Recycler:
    case sim::UnitKind::DeployedRecycler:
        return "RECYCLER";
    }
    return "UNIT";
}

namespace {
constexpr std::array<std::string_view, kRowGroups> kEmpty{
    "You have no group F1.", "You have no group F2.", "You have no group F3.",
    "You have no group F4.", "You have no group F5.", "You have no group F6.",
    "You have no group F7.", "You have no group F8.", "You have no group F9.",
    "You have no group F10."};
} // namespace

std::string_view empty_key_message(int key) noexcept {
    return key >= 1 && key <= kRowGroups ? kEmpty.at(static_cast<std::size_t>(key - 1))
                                         : std::string_view{};
}

} // namespace protekt::game
