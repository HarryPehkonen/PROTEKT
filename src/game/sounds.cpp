#include "protekt/game/sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "protekt/game/scene.hpp"

namespace protekt::game {

namespace {

using audio::SoundKind;
using audio::Voice;

/// A unit's place in the world, in tiles, taken from the table. Null for a unit that is absent or
/// dead: it has nowhere to be heard from.
const sim::Unit* living(std::span<const sim::Unit> units, sim::UnitId id) noexcept {
    for (const sim::Unit& unit : units) {
        if (unit.id == id) {
            return unit.alive ? &unit : nullptr;
        }
    }
    return nullptr;
}

/// Where a sound's source is relative to the listener: how far away in tiles, and how far to the
/// right in the listener's own frame (-1 left .. +1 right, as a fraction of the distance).
struct Bearing {
    float tiles{};
    float pan{};
};

/// The same camera frame the projection and the radar use: forward at yaw 0 is +Z, right is
/// (-cos, sin). The pan is the right component over the distance, which is the sine of the angle
/// off the nose -- 0 dead ahead, +-1 abeam, and 0 again dead behind, because a sound with nothing
/// to either side has nothing to pan to. (Ahead and behind are therefore not told apart by ear;
/// that is the limit of left/right.)
Bearing bearing_of(const Listener& listener, const sim::Unit& source) noexcept {
    constexpr float kRaw = 1.0F / 65536.0F;
    const float dx = (static_cast<float>(source.x.raw) * kRaw) - (listener.x / kMetresPerTile);
    const float dz = (static_cast<float>(source.y.raw) * kRaw) - (listener.z / kMetresPerTile);
    const float tiles = std::sqrt((dx * dx) + (dz * dz));
    if (!(tiles > 1e-4F)) {
        return Bearing{0.0F, 0.0F};
    }
    const float right = (dx * -std::cos(listener.yaw)) + (dz * std::sin(listener.yaw));
    return Bearing{tiles, std::clamp(right / tiles, -1.0F, 1.0F)};
}

/// 1 at the listener, falling with the square of the distance to exactly 0 at the earshot. The
/// square keeps nearby fights near full volume and lets the far edge fade out rather than cut.
float gain_at(float tiles, float earshot) noexcept {
    const float ratio = tiles / earshot;
    return std::clamp(1.0F - (ratio * ratio), 0.0F, 1.0F);
}

/// A sound from out in the world: attenuated by distance, panned by direction, and absent
/// altogether beyond `earshot`.
bool positioned(SoundKind kind, const Listener& listener, const sim::Unit& source, float earshot,
                std::vector<Voice>& out) {
    const Bearing where = bearing_of(listener, source);
    if (where.tiles >= earshot) {
        return false;
    }
    out.push_back(Voice{kind, gain_at(where.tiles, earshot), where.pan});
    return true;
}

bool lost_hit_points(const sim::BattleEvent& event, std::span<const Health> before) noexcept {
    for (const Health& health : before) {
        if (health.id == event.subject) {
            return event.subject_hp < health.hp;
        }
    }
    return false; // no record of what it had: nothing to say it lost any
}

} // namespace

std::vector<Voice> sounds_for(std::span<const sim::BattleEvent> events, const Listener& listener,
                              std::span<const sim::Unit> units,
                              std::span<const Health> health_before) noexcept {
    std::vector<Voice> voices;
    for (const sim::BattleEvent& event : events) {
        switch (event.kind) {
        case sim::EventKind::Ordered:
            break; // an order is a decision, not a noise
        case sim::EventKind::Fired: {
            const sim::Unit* actor = living(units, event.actor);
            if (actor == nullptr) {
                break;
            }
            if (actor->side == listener.side) {
                // Your own gun is the loudest thing you hear and needs no panning: it is your gun.
                // A friend out past earshot is silent like any other source -- the promise has no
                // exception for the right colour.
                if (bearing_of(listener, *actor).tiles < kEarshotTiles) {
                    voices.push_back(Voice{SoundKind::YourShot, 1.0F, 0.0F});
                }
                break;
            }
            const sim::Unit* subject = living(units, event.subject);
            if (subject != nullptr && subject->side == listener.side
                && lost_hit_points(event, health_before)) {
                // It HIT you: full gain and dead centre. The thing that is hurting you is not
                // positioned or faded; you need to know it, not where to look for it.
                voices.push_back(Voice{SoundKind::HitTaken, 1.0F, 0.0F});
                break;
            }
            // A hostile shot that hurt nobody of yours: heard as a distant gun, from its direction.
            (void)positioned(SoundKind::YourShot, listener, *actor, kEarshotTiles, voices);
            break;
        }
        case sim::EventKind::Killed: {
            const sim::Unit* subject = living(units, event.subject);
            if (subject == nullptr) {
                break;
            }
            if (subject->side == listener.side) {
                voices.push_back(Voice{SoundKind::Explosion, 1.0F, 0.0F}); // your loss
                break;
            }
            (void)positioned(SoundKind::Explosion, listener, *subject, kExplosionEarshotTiles,
                             voices);
            break;
        }
        }
    }
    return voices;
}

} // namespace protekt::game
