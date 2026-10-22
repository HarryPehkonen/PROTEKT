#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "protekt/sim/sim.hpp"

/// THE ROSTER (N2): your own units by number, from anywhere.
///
/// Digits 1-9 select your own units in SPAWN ORDER, whether or not the unit is on screen, and a
/// strip along the top of the screen lists them with their keys. Nothing here is stored: the number
/// is a unit's POSITION among its side's units in `Sim::units()`, and that list is append-only --
/// a unit that dies stays in it with `alive == false`, nothing is ever erased, nothing is inserted
/// before the end. So a number can never be reassigned: a death leaves its position (and its key)
/// dead, and every other unit is exactly where it was. The sim is not asked to change.
namespace protekt::game {

/// Digits 1-9. A tenth own unit has no key.
inline constexpr int kRosterKeys = 9;

struct RosterSlot {
    int key{}; ///< 1-9
    sim::UnitId id{};
    sim::UnitKind kind{};
    std::uint16_t hp{};
    std::uint16_t max_hp{};
};

/// The strip: the LIVING units of `side` that hold a key, in spawn order (so ascending key), and
/// how many living units are past the ninth and hold none. A dead unit is not here at all.
struct Roster {
    std::vector<RosterSlot> slots;
    int unkeyed{0};
};

[[nodiscard]] Roster roster_of(const sim::Sim& sim, sim::Side side);

/// What a number means right now.
enum class KeyState : std::uint8_t {
    Alive, ///< a living unit holds it: `id`
    Dead,  ///< a unit held it and has been destroyed
    Never  ///< nobody ever held it
};
struct KeyLookup {
    KeyState state{KeyState::Never};
    sim::UnitId id{};
};
[[nodiscard]] KeyLookup unit_for_key(const sim::Sim& sim, sim::Side side, int key);

/// The strip's short name for a kind, in the capitals the HUD font has.
[[nodiscard]] std::string_view kind_label(sim::UnitKind kind) noexcept;

/// What pressing a dead or empty key says. Literals, because a message is a view.
[[nodiscard]] std::string_view dead_key_message(int key) noexcept;
[[nodiscard]] std::string_view empty_key_message(int key) noexcept;

} // namespace protekt::game
