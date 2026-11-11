#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "protekt/game/faction.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/sim/sim.hpp"

/// THE COMMAND ROW (ROW-SLOTS): `~`, `1`, `2`, `3`, then `F1`..`F10`, fourteen keys that each sit
/// in the same spot on the screen for the whole game.
///
/// The ruler's rules (2026-10-24, from a Battlezone: Combat Commander capture):
///  * A key that holds nothing is not drawn, and the row does not close up: its space stays blank.
///    A slot's index is fixed forever -- slot 4 is F1, slot 13 is F10.
///  * `1` is the deployed recycler (DEPLOY) and `2` the factory; `3` (the armory) has no kind yet,
///    so it is empty. `~` is the nav marks. A recycler that deploys leaves its F key blank, in
///    place, and appears on `1`.
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
/// A vehicle arrives carrying the batch its build order named: a lone build is a batch of one, and
/// every unit built from one shift gesture carries that gesture's token. `update` gives a batch the
/// slot its FIRST member takes, so a shift-built batch shares one F key (the ruling) while two lone
/// builds take one key each. Scenario-placed units belong to no batch and are placed one per key --
/// except at the START: `seed_from_start` seats the opening force one key per KIND (his ruling,
/// 2026-10-29), so every key is one kind from tick 0 and a shift-move can be refused by kind.
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
    /// The living units held here, in spawn order. Empty means the key is not drawn -- unless
    /// `batch` holds it.
    std::vector<sim::UnitId> units{};
    /// THE BATCH THIS KEY IS HELD FOR (BATCH-PLATE): the batch id of the build that is
    /// still under way, or 0: a SHIFT run's token, or a LONE build's own id (a lone build is a
    /// batch of one). The key is taken the moment the first entry is queued,
    /// before any unit exists, so the plate can show the count as blank ticks and the icon filling;
    /// the units land on it as they are built. It is let go the frame the queue holds no more of
    /// the batch.
    std::uint32_t batch{0};

    [[nodiscard]] bool operator==(const RowSlot&) const = default;
};

/// What `RowState::move_into` did. Anything but `Moved` changed NOTHING, and says why.
enum class RowMove : std::uint8_t {
    Moved,         ///< the units are in the key now
    NothingToMove, ///< the selection is empty, or none of it is a living unit the row holds
    MixedKinds,    ///< the selection spans more than one kind, and a key holds one
    DifferentKind, ///< the target key holds a different kind from the moved units
    NoSuchKey,     ///< not F1..F10
};

struct RowState {
    std::array<RowSlot, kRowSlots> slots{}; ///< ~ 1 2 3 F1..F10, in that fixed order
    std::size_t nav_marks{0};               ///< `~` is on the screen when this is above zero
    /// THE START RULE, once, at tick 0, on a row that has seen nothing: seat `side`'s opening force
    /// ONE GROUP KEY PER KIND. All the tanks share F1, and each other kind of vehicle takes the
    /// next empty key, in the order its first unit spawns -- the turrets too, which always sit
    /// under a key (2026-11-02); the factory and the deployed recycler never did. The undeployed
    /// recycler moves and deploys from its F key, so it is a kind like any. `driven` is the
    /// vehicle the player starts in, and it is seated nowhere (see `update`). Then it fills the
    /// building keys and `~` as `update` does. This is the ONLY place units are grouped by kind:
    /// every unit that appears afterwards goes through `update`'s per-unit rules (a lone build
    /// takes the first empty key, a shift-built batch shares its first member's, a wiped-out key is
    /// reused first).
    void seed_from_start(const sim::Sim& sim, sim::Side side, std::size_t marks,
                         sim::UnitId driven = {});

    /// Fold in this frame's world: prune the dead, hand every vehicle `side` has not had before the
    /// first empty group slot, and refill the building keys. `nav_marks` is how many marks the
    /// player has set; `~` is on the screen when there is at least one.
    ///
    /// `driven` is THE VEHICLE THE PLAYER IS IN, and it is never under a key (Harri, 2026-11-02):
    /// the keys are for assets he commands, he already controls that one, and a key for it could
    /// only offer an order that fails. It is pruned from any key that held it and never seated.
    /// The default, no vehicle, is an observer's row.
    ///
    /// The group keys are therefore not a function of the sim alone: they remember where the
    /// player put things (`move_into`), and `update` keeps those places -- it only prunes the dead
    /// and seats newcomers in the gaps. A build (a shift run, or a lone one) still in a producer's
    /// queue is held a key too
    /// (`RowSlot::batch`): the one a member already sits on, else the first empty group key, taken
    /// the frame its first entry is queued and kept until the queue holds no more of it.
    void update(const sim::Sim& sim, sim::Side side, std::size_t marks, sim::UnitId driven = {});

    /// THE SWAP (X): the player leaves `vacated` for `entered`. The row is re-derived the way he
    /// ruled: `entered` loses its key, and `vacated` takes the first empty one. DISALLOWED when
    /// there is none for it -- a false return, and then nothing has changed. A key `entered` held
    /// alone is empty the moment it is entered, so it counts as the free one. A `vacated` that is
    /// dead, unknown, or already on a key needs none.
    [[nodiscard]] bool swap_driven(const sim::Sim& sim, sim::UnitId vacated, sim::UnitId entered);

    /// Whether the key at `index` is on the screen: it holds something, or is held for a batch.
    [[nodiscard]] bool occupied(std::size_t index) const;

    /// SHIFT + F`group_key`: move `units` (a selection) into function key `group_key` (1-10). The
    /// target keeps its units and gains the moved ones AFTER them, in the order given (a
    /// selection's is spawn order); every group slot that held a moved unit loses it, and one
    /// emptied goes blank where it stands. Nothing else in the row moves, and the target may be
    /// empty. A unit moves only if it is alive and a group slot holds it, so an enemy, a building
    /// key's occupant, an id the row has never seen and a corpse are all skipped. A unit already
    /// in the target stays where it is rather than going to the back.
    ///
    /// Refused, and then NOTHING changes: no unit to move; the units are of more than one kind (a
    /// key holds one); the target holds a different kind, or is held for a batch of another
    /// kind. `Moved` otherwise.
    RowMove move_into(const sim::Sim& sim, std::span<const sim::UnitId> units, int group_key);
};

/// What a drawn unit-holding slot reads, at this tick: its living units and their hit points.
struct SlotReading {
    sim::UnitKind kind{};   ///< the first living unit's: a group is one kind
    std::size_t count{};    ///< living units
    std::uint16_t hp{};     ///< summed over them
    std::uint16_t max_hp{}; ///< summed over them
    /// One per living unit, in slot order (spawn order, until a move appends to the key): its hit
    /// points over its kind's ceiling.
    /// A corpse contributes none, so `fractions.size() == count`.
    std::vector<float> fractions{};
    /// The same living units, in the same order: `units.at(i)` is the vehicle `fractions.at(i)`
    /// reads, so the tick drawn for it can be told from the others (`picked_ticks`).
    std::vector<sim::UnitId> units{};
};

/// THE LIST `~` OPENS ON A FUNCTION KEY (AV): one row per living unit the slot's health bar draws,
/// in the bars' own order (`read_slot`'s -- this is built FROM it, so the two cannot disagree),
/// keyed 1-9 then 0 for the tenth. At most `kAttackListRows` rows: a key holding more lists the
/// first ten, as only ten digits exist. Empty when nothing in the slot is alive. `metres` is 0.
[[nodiscard]] std::vector<TargetRow> unit_list_rows(const sim::Sim& sim, const RowSlot& slot);

/// One vehicle's piece of a key's health bar (BAR-LOOK). HIS RULING, 2026-10-24: one bar per
/// vehicle, varying in COLOUR, not length -- bright green when whole, then yellow, red and black.
struct BarSegment {
    float fraction{};           ///< the vehicle's, clamped to 0..1
    std::array<float, 3> rgb{}; ///< `fraction` on the ramp
};

/// One segment per fraction, in the order given, coloured on a continuous ramp through four
/// anchors: 1.0 bright green, 0.66 yellow, 0.33 red, 0.0 near-black. The floor is deliberately not
/// pure black: the plate behind the bar is black, and a vehicle about to die must still read as a
/// bar rather than as no vehicle. Pure, so the look is pinned by tests and not by eye.
[[nodiscard]] std::vector<BarSegment> bar_segments(std::span<const float> fractions);

/// The slot read against the sim as it is now, or nullopt when none of its units is alive -- so a
/// frame drawn before the row is next updated still never shows a dead key.
[[nodiscard]] std::optional<SlotReading> read_slot(const sim::Sim& sim, const RowSlot& slot);

/// WHICH TICKS OF A KEY ARE PICKED OUT (`~` STEPPING): the positions in `reading` (the same order
/// as its `fractions`) of the living vehicles in `selected`, when that is SOME of the key and not
/// all of it. Empty when none is selected or the whole group is -- a whole group is the key's
/// lit frame already, and a tick marked on every vehicle would say nothing.
[[nodiscard]] std::vector<std::size_t> picked_ticks(const SlotReading& reading,
                                                    std::span<const sim::UnitId> selected);

/// What a key reads of the batch still being built for it (BATCH-PLATE): read off the producers'
/// own queues (`sim::BuildQueue`), never stored.
struct BatchReading {
    sim::UnitKind kind{}; ///< what is being built: a queue is one batch, so one kind
    std::size_t queued{}; ///< entries still in the queue, the one on the line included
    /// How far the front entry is through its build, 0..1: `1 - left / build_ticks`. Zero while it
    /// waits for its scrap (a build that has not started has nothing in it to show).
    float fill{};
};

/// The batch being built for `slot` by a producer of `side`, or nullopt when none is: a queue
/// whose front entry carries a batch id that the slot holds (`RowSlot::batch`) or that one of the
/// slot's units was built under -- a shift token, or a lone build's own id.
[[nodiscard]] std::optional<BatchReading> read_batch(const sim::Sim& sim, sim::Side side,
                                                     const RowSlot& slot);

/// WHO BUILDING KEY `key` (1 or 2) BRINGS UP: the lowest-id living building of `side` that the row
/// draws under that key, or an invalid id when it holds none (and for every other key -- `3`, the
/// armory, has no kind yet). It answers from the sim through the same kind-to-key mapping `update`
/// fills the row with, so the key's reach and the key's drawing cannot disagree. An UNDEPLOYED
/// recycler is still "the recycler": it sits under an F key until it deploys, but key `1` finds it
/// when no deployed one is alive, so the key does not care which side of the deployment it is on.
[[nodiscard]] sim::UnitId building_key_holder(const sim::Sim& sim, sim::Side side, int key);

/// The short name of a kind, in the capitals the HUD font has, keyed by (kind, FACTION): the one
/// name table every readout, command row and list reads. A faction names the machine -- the human
/// Scavenger is the alien Harvester, the Turret a Guardian, the Tug a Hauler -- and never the side,
/// so a caller passes the faction of the unit's own side. A DEPLOYED miner is an EXTRACTOR to both.
[[nodiscard]] std::string_view kind_label(sim::UnitKind kind, Faction faction) noexcept;

/// What pressing F`key` says when its slot holds nothing. A literal, because a message is a view.
[[nodiscard]] std::string_view empty_key_message(int key) noexcept;

/// What a refused move says on the notice line, or empty for `Moved` and `NoSuchKey`. Literals.
[[nodiscard]] std::string_view move_refusal_message(RowMove outcome) noexcept;

} // namespace protekt::game
