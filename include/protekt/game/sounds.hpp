#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "protekt/audio/mixer.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/units.hpp"

/// What the player hears: a PURE RULE from the battle's events to a list of voices. It makes no
/// sound and holds no device; `audio::mix` turns the voices into samples and `app::AudioOut` plays
/// them. The simulation never learns that any of this exists -- the events it reads are the same
/// ones the replay log is built from, derived by diffing states after the fact.
///
/// BEYOND EARSHOT, NOTHING. Not quiet: not there. It is the promise the radar makes about what is
/// out of range, and for the same reason -- a file of enemy assets the player cannot perceive must
/// not be audible either, or the radius is decoration and the ears are a map.
namespace protekt::game {

/// Where the player is listening from. Metres, because that is what the camera is in.
struct Listener {
    float x{}; ///< world metres
    float z{};
    float yaw{}; ///< radians, as the camera's yaw
    sim::Side side{sim::Side::Player};
};

/// What a unit's hit points were BEFORE the tick being reported. A hit is a shot that took hit
/// points off someone on the listener's side; the event alone says what the target has left, not
/// whether the shot cost it anything.
struct Health {
    sim::UnitId id{};
    std::uint16_t hp{};
};

inline constexpr float kEarshotTiles = 60.0F; ///< a shot you cannot hear is not audible
/// Explosions carry further: they are loud.
inline constexpr float kExplosionEarshotTiles = 90.0F;

/// The voices for one tick's events.
///
/// `units` is the table the events are ABOUT, so the unit that died this tick must still be in it:
/// pass the units as they stood before the tick. A unit that is dead in that table, or absent from
/// it, contributes nothing -- no position to hear it from.
[[nodiscard]] std::vector<audio::Voice> sounds_for(std::span<const sim::BattleEvent> events,
                                                   const Listener& listener,
                                                   std::span<const sim::Unit> units,
                                                   std::span<const Health> health_before) noexcept;

} // namespace protekt::game
