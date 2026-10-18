// The rule from a battle's events to what the player hears. Pure: units and events in, voices out.
//
// The listener stands at the world origin of a tidy field, facing +Z at yaw 0 (so +X is to the
// LEFT, as the camera's right is (-cos, sin)). Everything is placed in tiles and converted, since
// the rule's distances are tiles and a listener is in metres.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/game/sounds.hpp"

namespace {

using protekt::audio::SoundKind;
using protekt::audio::Voice;
using protekt::game::Health;
using protekt::game::kEarshotTiles;
using protekt::game::kExplosionEarshotTiles;
using protekt::game::kMetresPerTile;
using protekt::game::Listener;
using protekt::game::sounds_for;
using protekt::sim::BattleEvent;
using protekt::sim::EventKind;
using protekt::sim::Fixed;
using protekt::sim::Side;
using protekt::sim::Unit;
using protekt::sim::UnitId;

constexpr float kCentreTile = 100.0F;

Unit unit_at(std::uint32_t id, Side side, float tile_x, float tile_z, std::uint16_t hp = 100) {
    Unit u{};
    u.id = UnitId{id};
    u.side = side;
    u.hp = hp;
    u.x.raw = static_cast<std::int32_t>(tile_x * static_cast<float>(Fixed::kOne));
    u.y.raw = static_cast<std::int32_t>(tile_z * static_cast<float>(Fixed::kOne));
    u.alive = true;
    return u;
}

Listener listener_at_centre(float yaw = 0.0F) {
    return Listener{kCentreTile * kMetresPerTile, kCentreTile * kMetresPerTile, yaw, Side::Player};
}

/// A unit `tiles` from the listener, straight ahead (+Z) at yaw 0.
Unit ahead(std::uint32_t id, Side side, float tiles) {
    return unit_at(id, side, kCentreTile, kCentreTile + tiles);
}

BattleEvent fired(std::uint32_t actor, std::uint32_t subject, std::uint16_t hp_after) {
    return BattleEvent{1, EventKind::Fired, UnitId{actor}, UnitId{subject}, hp_after};
}

BattleEvent killed(std::uint32_t actor, std::uint32_t subject) {
    return BattleEvent{1, EventKind::Killed, UnitId{actor}, UnitId{subject}, 0};
}

std::vector<Voice> hear(const std::vector<BattleEvent>& events, const std::vector<Unit>& units,
                        const std::vector<Health>& health, float yaw = 0.0F) {
    return sounds_for(events, listener_at_centre(yaw), units, health);
}

// ---- your side ----------------------------------------------------------------------------

TEST(Sounds, NoEventsMakeNoVoices) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 10.0F)};
    EXPECT_TRUE(hear({}, units, {{UnitId{1}, 100}}).empty());
}

TEST(Sounds, YourOwnShotIsFullGainAndCentred) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 10.0F)};
    const std::vector<Voice> v = hear({fired(1, 2, 92)}, units, {});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].kind, SoundKind::YourShot);
    EXPECT_EQ(v[0].gain, 1.0F);
    EXPECT_EQ(v[0].pan, 0.0F);
}

TEST(Sounds, AFriendBesideYouIsAlsoYourSideAndFullGain) {
    const std::vector<Unit> units{unit_at(3, Side::Player, kCentreTile - 8.0F, kCentreTile),
                                  ahead(2, Side::Enemy, 10.0F)};
    const std::vector<Voice> v = hear({fired(3, 2, 92)}, units, {});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].gain, 1.0F);
    EXPECT_EQ(v[0].pan, 0.0F);
}

TEST(Sounds, AFriendBeyondEarshotIsSilentToo) {
    const std::vector<Unit> units{ahead(3, Side::Player, kEarshotTiles + 5.0F),
                                  ahead(2, Side::Enemy, kEarshotTiles + 10.0F)};
    EXPECT_TRUE(hear({fired(3, 2, 92)}, units, {}).empty());
}

// ---- being hit ----------------------------------------------------------------------------

TEST(Sounds, AShotThatHitsYouIsHitTakenAtFullGainCentred) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 40.0F)};
    const std::vector<Voice> v = hear({fired(2, 1, 92)}, units, {{UnitId{1}, 100}});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].kind, SoundKind::HitTaken);
    EXPECT_EQ(v[0].gain, 1.0F) << "a hit is full gain whatever the distance it came from";
    EXPECT_EQ(v[0].pan, 0.0F) << "no panning on the thing that is hurting you";
}

TEST(Sounds, AHitFromTheSideIsStillCentred) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F),
                                  unit_at(2, Side::Enemy, kCentreTile + 12.0F, kCentreTile)};
    const std::vector<Voice> v = hear({fired(2, 1, 92)}, units, {{UnitId{1}, 100}});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].kind, SoundKind::HitTaken);
    EXPECT_EQ(v[0].pan, 0.0F);
}

TEST(Sounds, AHostileWhoseShotMissedMakesNoHitTaken) {
    // hp after == hp before: the shot cost nobody anything. The listener's own unit is the named
    // subject, and still it is not a hit.
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 20.0F)};
    const std::vector<Voice> v = hear({fired(2, 1, 100)}, units, {{UnitId{1}, 100}});
    for (const Voice& voice : v) {
        EXPECT_NE(voice.kind, SoundKind::HitTaken);
    }
    ASSERT_EQ(v.size(), 1U) << "it is still a gun going off nearby";
    EXPECT_EQ(v[0].kind, SoundKind::YourShot);
}

TEST(Sounds, AHostileShotAtSomeoneElseOfYoursIsNotYourHit) {
    // Subject on the listener's side but not the listener's own unit: still "on your side" by the
    // rule, so it is a hit taken. A shot at the OTHER side is not.
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 20.0F),
                                  ahead(3, Side::Enemy, 25.0F)};
    const std::vector<Voice> v = hear({fired(2, 3, 92)}, units, {{UnitId{3}, 100}});
    for (const Voice& voice : v) {
        EXPECT_NE(voice.kind, SoundKind::HitTaken) << "enemy hurting enemy is not your hit";
    }
}

TEST(Sounds, AHitWithNoHealthRecordIsNotClaimedAsAHit) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 20.0F)};
    const std::vector<Voice> v = hear({fired(2, 1, 92)}, units, {});
    for (const Voice& voice : v) {
        EXPECT_NE(voice.kind, SoundKind::HitTaken);
    }
}

// ---- a gun heard from out in the world ----------------------------------------------------

float gain_of_hostile_shot_at(float tiles) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, tiles),
                                  ahead(4, Side::Enemy, tiles + 3.0F)};
    const std::vector<Voice> v = hear({fired(2, 4, 92)}, units, {{UnitId{4}, 100}});
    return v.empty() ? -1.0F : v[0].gain;
}

TEST(Sounds, ADistantHostileShotIsQuieterThanANearOne) {
    const float near_gain = gain_of_hostile_shot_at(10.0F);
    const float far_gain = gain_of_hostile_shot_at(45.0F);
    ASSERT_GE(near_gain, 0.0F);
    ASSERT_GE(far_gain, 0.0F);
    EXPECT_GT(near_gain, far_gain);
    EXPECT_NEAR(near_gain, 1.0F - ((10.0F / kEarshotTiles) * (10.0F / kEarshotTiles)), 1e-3F);
    EXPECT_NEAR(far_gain, 1.0F - ((45.0F / kEarshotTiles) * (45.0F / kEarshotTiles)), 1e-3F);
}

TEST(Sounds, AShotBeyondEarshotProducesNoVoiceAtAll) {
    // Not a quiet voice: no voice. The promise the radar makes, made to the ears.
    EXPECT_LT(gain_of_hostile_shot_at(kEarshotTiles + 0.5F), 0.0F);
    EXPECT_LT(gain_of_hostile_shot_at(kEarshotTiles + 40.0F), 0.0F);
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F),
                                  ahead(2, Side::Enemy, kEarshotTiles + 5.0F)};
    EXPECT_TRUE(hear({fired(2, 1, 100)}, units, {{UnitId{1}, 100}}).empty());
}

TEST(Sounds, JustInsideEarshotIsAudibleButNearlySilent) {
    const float g = gain_of_hostile_shot_at(kEarshotTiles - 0.5F);
    ASSERT_GE(g, 0.0F) << "the edge is inside";
    EXPECT_LT(g, 0.05F);
}

TEST(Sounds, AShotIsPannedByWhereItCameFrom) {
    // Yaw 0 faces +Z; the camera's right is (-cos, sin) = -X. So +X is to the left.
    const auto pan_of = [](float dx, float dz, float yaw) {
        const std::vector<Unit> units{ahead(1, Side::Player, 0.0F),
                                      unit_at(2, Side::Enemy, kCentreTile + dx, kCentreTile + dz),
                                      ahead(4, Side::Enemy, 3.0F)};
        const std::vector<Voice> v = hear({fired(2, 4, 92)}, units, {{UnitId{4}, 100}}, yaw);
        return v.empty() ? 99.0F : v[0].pan;
    };
    EXPECT_NEAR(pan_of(0.0F, 20.0F, 0.0F), 0.0F, 1e-4F) << "dead ahead: no panning";
    EXPECT_NEAR(pan_of(0.0F, -20.0F, 0.0F), 0.0F, 1e-4F) << "dead behind: no panning";
    EXPECT_NEAR(pan_of(-20.0F, 0.0F, 0.0F), 1.0F, 1e-4F) << "abeam to the right";
    EXPECT_NEAR(pan_of(20.0F, 0.0F, 0.0F), -1.0F, 1e-4F) << "abeam to the left";
    EXPECT_NEAR(pan_of(-14.1421F, 14.1421F, 0.0F), 0.7071F, 1e-3F) << "ahead and to the right";
    // Turning to face the shooter takes the pan away: the same unit, a different listener.
    const float quarter_turn = 3.14159265F / 2.0F;
    EXPECT_GT(std::fabs(pan_of(0.0F, 20.0F, quarter_turn)), 0.99F);
}

TEST(Sounds, AHostileShotUsesTheHostileSidePositionNotYours) {
    // The distance is to the SHOOTER, not to the unit it shot at.
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 50.0F),
                                  ahead(4, Side::Enemy, 5.0F)};
    const std::vector<Voice> v = hear({fired(2, 4, 92)}, units, {{UnitId{4}, 100}});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_NEAR(v[0].gain, 1.0F - ((50.0F / kEarshotTiles) * (50.0F / kEarshotTiles)), 1e-3F);
}

// ---- kills --------------------------------------------------------------------------------

TEST(Sounds, AKillOnYourSideIsAFullGainExplosionWhereverItHappened) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F),
                                  ahead(3, Side::Player, kExplosionEarshotTiles + 50.0F),
                                  ahead(2, Side::Enemy, 5.0F)};
    const std::vector<Voice> v = hear({killed(2, 3)}, units, {});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].kind, SoundKind::Explosion);
    EXPECT_EQ(v[0].gain, 1.0F);
    EXPECT_EQ(v[0].pan, 0.0F);
}

TEST(Sounds, AnEnemyKillCarriesFurtherThanAShot) {
    // 75 tiles: past a shot's earshot (60), inside an explosion's (90).
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Player, 74.0F),
                                  ahead(5, Side::Enemy, 75.0F)};
    const std::vector<Voice> v = hear({killed(2, 5)}, units, {});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].kind, SoundKind::Explosion);
    EXPECT_NEAR(v[0].gain,
                1.0F - ((75.0F / kExplosionEarshotTiles) * (75.0F / kExplosionEarshotTiles)),
                1e-3F);
}

TEST(Sounds, AnEnemyKillBeyondTheExplosionEarshotMakesNoVoice) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Player, 0.0F),
                                  ahead(5, Side::Enemy, kExplosionEarshotTiles + 1.0F)};
    EXPECT_TRUE(hear({killed(2, 5)}, units, {}).empty());
}

TEST(Sounds, AnEnemyKillIsPannedLikeAShot) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Player, 0.0F),
                                  unit_at(5, Side::Enemy, kCentreTile - 20.0F, kCentreTile)};
    const std::vector<Voice> v = hear({killed(2, 5)}, units, {});
    ASSERT_EQ(v.size(), 1U);
    EXPECT_NEAR(v[0].pan, 1.0F, 1e-4F);
}

// ---- who counts ---------------------------------------------------------------------------

TEST(Sounds, AMissingOrDeadUnitContributesNothing) {
    std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 10.0F)};
    EXPECT_TRUE(hear({fired(99, 1, 92)}, units, {{UnitId{1}, 100}}).empty()) << "unknown actor";
    units[1].alive = false;
    EXPECT_TRUE(hear({fired(2, 1, 92)}, units, {{UnitId{1}, 100}}).empty()) << "dead actor";
    EXPECT_TRUE(hear({killed(1, 77)}, units, {}).empty()) << "unknown subject of a kill";
}

TEST(Sounds, OrdersAreNotNoises) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F)};
    const BattleEvent order{1, EventKind::Ordered, UnitId{1}, UnitId{}, 0};
    EXPECT_TRUE(hear({order}, units, {}).empty());
}

TEST(Sounds, SeveralEventsMakeSeveralVoicesInOrder) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 20.0F)};
    const std::vector<Voice> v
        = hear({fired(1, 2, 92), fired(2, 1, 92), killed(1, 2)}, units, {{UnitId{1}, 100}});
    ASSERT_EQ(v.size(), 3U);
    EXPECT_EQ(v[0].kind, SoundKind::YourShot);
    EXPECT_EQ(v[1].kind, SoundKind::HitTaken);
    EXPECT_EQ(v[2].kind, SoundKind::Explosion);
}

TEST(Sounds, TheOtherSidesListenerHearsTheMirror) {
    const std::vector<Unit> units{ahead(1, Side::Player, 0.0F), ahead(2, Side::Enemy, 10.0F)};
    Listener enemy = listener_at_centre();
    enemy.side = Side::Enemy;
    const std::vector<Health> health{{UnitId{2}, 100}};
    const std::vector<Voice> v
        = sounds_for(std::vector<BattleEvent>{fired(1, 2, 92)}, enemy, units, health);
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0].kind, SoundKind::HitTaken);
}

} // namespace
