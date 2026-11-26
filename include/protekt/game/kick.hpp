#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "protekt/audio/synth.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

/// A3, RECOIL: what a shot does to the player's senses. The sim's half -- the HEAVY class moving a
/// hull -- is `sim::kKickSpecs` and `Sim::step` 4a; this is the other half, and it is presentation
/// only: nothing here is given to the sim, and a battle hashes the same whether or not it is used.
///
/// One record per shot, `FireCue`, read off the sim's own `Sim::Shot`: the weapon that fired, its
/// kickback class, and the sound it makes. The sound travels WITH the shot rather than being chosen
/// somewhere else from the same event, so a shot with no sound is a row of `kFireSounds` that says
/// so, and a test can hold every armed weapon to having one.
namespace protekt::game {

/// THE SOUND EACH WEAPON MAKES, one row per `sim::WeaponId`; unarmed has none. Today every gun
/// makes the one shot the synthesiser has (`audio::SoundKind::YourShot`): the tree's sound is
/// arithmetic, with no files, and a heavier report for the heavy class, an echo or a reverberation
/// are a later slice's. This table is where each weapon will get its own -- and the coupling it
/// feeds is in place now.
inline constexpr std::array<std::optional<audio::SoundKind>, sim::kWeaponCount> kFireSounds{{
    std::nullopt,               // none
    audio::SoundKind::YourShot, // tankgun
    audio::SoundKind::YourShot, // turretgun
    audio::SoundKind::YourShot, // minigun
    audio::SoundKind::YourShot, // mortar
    audio::SoundKind::YourShot, // rocket
    audio::SoundKind::YourShot, // shotgun
    audio::SoundKind::YourShot, // walkergun
    audio::SoundKind::YourShot, // towergun
}};

[[nodiscard]] constexpr std::optional<audio::SoundKind> fire_sound(sim::WeaponId weapon) noexcept {
    return kFireSounds.at(static_cast<std::size_t>(weapon));
}

/// What one shot carries to the player: who fired, with what, how it kicks, and how it sounds.
struct FireCue {
    sim::UnitId shooter{};
    sim::WeaponId weapon{};
    sim::Kick kick{};
    std::optional<audio::SoundKind> sound{};
};

/// The cue for a shot the sim made: its weapon's class from `sim::kWeaponSpecs` and its sound from
/// `kFireSounds`, nothing chosen here.
[[nodiscard]] FireCue cue_of(const sim::Sim::Shot& shot) noexcept;

/// THE LOOK OF A KICK, per class: how far the view heaves UP at the instant of the shot (radians
/// of pitch), how far the world goes toward the muzzle's light (0..1 of `kMuzzleFlashColour`), and
/// how many ticks both take to die away. PROVISIONAL, Harri's to judge by eye. Light: two degrees
/// and a faint flash over a sixth of a second -- a jolt, not a lurch. Heavy: twice the heave, a
/// brighter flash, held near twice as long; its hull is also being carried back by the sim, so
/// the view moves with it.
struct KickLook {
    float heave_radians{};
    float flash{};
    sim::Tick ticks{};
};
inline constexpr std::array<KickLook, sim::kKickCount> kKickLooks{{
    {0.035F, 0.16F, 5}, // light
    {0.070F, 0.28F, 9}, // heavy
}};

/// The muzzle's light, warm white: what the world is washed toward on the flash.
inline constexpr std::array<float, 3> kMuzzleFlashColour{1.0F, 0.92F, 0.70F};

/// THE VIEW'S KICK, owned by the app like `HitFlash`. `kick` arms it from the top with a shot's
/// class; `heave` and `flash` are the class's full amount on the tick of the shot, fall linearly,
/// and are exactly 0 from `KickLook::ticks` later on. A second shot re-arms it.
class ViewKick {
public:
    void kick(sim::Kick kick, sim::Tick now) noexcept;
    [[nodiscard]] float heave(sim::Tick now) const noexcept;
    [[nodiscard]] float flash(sim::Tick now) const noexcept;

private:
    [[nodiscard]] float level(sim::Tick now) const noexcept;
    sim::Tick at_{};
    sim::Kick kick_{};
    bool armed_{false};
};

/// The camera with the view heaved up by `heave` radians of pitch, and nothing else: the eye's
/// position, its yaw and its frame are untouched, and so is the sim. Capped short of straight up.
[[nodiscard]] Camera kicked_camera(Camera camera, float heave) noexcept;

} // namespace protekt::game
