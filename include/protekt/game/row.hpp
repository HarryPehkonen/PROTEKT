#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "protekt/sim/sim.hpp"

/// THE COMMAND ROW (ROW-SLOTS): `~`, `1`, `2`, `3`, then `F1`..`F10`, fourteen keys that each sit
/// in the same spot on the screen for the whole game.
///
/// The ruler's rules (2026-10-24, from a Battlezone: Combat Commander capture):
///  * A key that holds nothing is not drawn, and the row does not close up: its space stays blank.
///    A slot's index is fixed forever -- slot 4 is F1, slot 13 is F10.
///  * `2` is the factory; `1` (the deployed recycler) and `3` (the armory) have no kind yet, so
///    they are empty. `~` is the nav marks.
///  * Buildings sit on the numbered keys only. F1..F10 hold GROUPS OF VEHICLES, not kinds: one
///    kind may hold several keys, and a key may hold a single vehicle.
///  * A unit not seen before takes the FIRST EMPTY group slot, lowest index first. A slot whose
///    units are all gone is empty but keeps its place, so the next new unit fills the gap rather
///    than the row re-packing.
///
/// This replaces K1's pool, which was a pure function of the living units and so could not
/// remember a key. The row remembers, and the memory lives here in the game layer: the sim is
/// untouched, and no tick or hash depends on it. Whoever owns the row calls `update` once per
/// frame, after the sim has stepped.
///
/// For now every vehicle is its own one-unit group, because nothing builds a batch yet. When a
/// shift-built batch exists (it shares one F key while a lone build takes its own) the unit will
/// arrive with a batch id, and `update` will give a batch the slot its first member takes.
namespace protekt::game {

/// What a position in the row IS. Fixed by the index; see `row_slot_kind`.
enum class RowSlotKind : std::uint8_t { Navs, RecyclerKey, FactoryKey, ArmoryKey, Group };

inline constexpr std::size_t kRowSlots = 14;     ///< ~ 1 2 3 F1 F2 F3 F4 F5 F6 F7 F8 F9 F10
inline constexpr std::size_t kRowFirstGroup = 4; ///< the index of F1
inline constexpr int kRowGroups = static_cast<int>(kRowSlots - kRowFirstGroup); ///< F1..F10

[[nodiscard]] constexpr RowSlotKind row_slot_kind(std::size_t index) noexcept {
    constexpr std::array<RowSlotKind, kRowFirstGroup> kFixed{
        RowSlotKind::Navs, RowSlotKind::RecyclerKey, RowSlotKind::FactoryKey,
        RowSlotKind::ArmoryKey};
    return index < kRowFirstGroup ? kFixed.at(index) : RowSlotKind::Group;
}

/// What the key is called on the screen: "~", "1", "2", "3", "F1".."F10".
[[nodiscard]] std::string_view row_key_label(std::size_t index) noexcept;

/// The slot index of function key `n` (1-10).
[[nodiscard]] constexpr std::size_t group_slot(int n) noexcept {
    return kRowFirstGroup + static_cast<std::size_t>(n - 1);
}

struct RowSlot {
    /// The living units held here, in spawn order. Empty means the key is not drawn.
    std::vector<sim::UnitId> units{};
};

struct RowState {
    std::array<RowSlot, kRowSlots> slots{}; ///< ~ 1 2 3 F1..F10, in that fixed order
    std::size_t nav_marks{0};               ///< `~` is on the screen when this is above zero

    /// Fold in this frame's world: prune the dead, hand every vehicle `side` has not had before the
    /// first empty group slot, and refill the building keys. `nav_marks` is how many marks the
    /// player has set; `~` is on the screen when there is at least one.
    void update(const sim::Sim& sim, sim::Side side, std::size_t marks);

    /// Whether the key at `index` is on the screen: it holds something.
    [[nodiscard]] bool occupied(std::size_t index) const;
};

/// What a drawn unit-holding slot reads, at this tick: its living units and their hit points.
struct SlotReading {
    sim::UnitKind kind{};   ///< the first living unit's: a group is one kind
    std::size_t count{};    ///< living units
    std::uint16_t hp{};     ///< summed over them
    std::uint16_t max_hp{}; ///< summed over them
};

/// The slot read against the sim as it is now, or nullopt when none of its units is alive -- so a
/// frame drawn before the row is next updated still never shows a dead key.
[[nodiscard]] std::optional<SlotReading> read_slot(const sim::Sim& sim, const RowSlot& slot);

/// The strip's short name for a kind, in the capitals the HUD font has.
[[nodiscard]] std::string_view kind_label(sim::UnitKind kind) noexcept;

/// What pressing F`key` says when its slot holds nothing. A literal, because a message is a view.
[[nodiscard]] std::string_view empty_key_message(int key) noexcept;

} // namespace protekt::game
