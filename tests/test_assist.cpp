// ASSIST-TARGETTING (`T`): the hostile nearest the crosshair within a few degrees is latched, and
// the scene draws a thin red line from it to a small red dot -- the point to aim the BARREL at for
// the weapon that is up to hit it if it holds its course. The dot draws; it never steers.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/cmd/command.hpp"
#include "protekt/game/assist.hpp"
#include "protekt/game/attack_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/world.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};
constexpr float kRadiansPerDegree = math::kPi / 180.0F;
constexpr float kOne = static_cast<float>(sim::Fixed::kOne);

[[nodiscard]] float tiles_of(sim::Fixed value) { return static_cast<float>(value.raw) / kOne; }

[[nodiscard]] sim::Fixed to_fixed(float value) {
    return sim::Fixed::from_raw(
        static_cast<std::int32_t>(std::lround(static_cast<double>(value) * kOne)));
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

// ------------------------------------------------------------------------------ the cone rig

struct Field {
    sim::Sim sim;
    SceneOptions options;
    UnitId driven;
    UnitId enemy;
    UnitId friendly;
    UnitId turret;
};

/// The camera stands 20 tiles short of the enemy tank, level with its middle, looking straight at
/// it (yaw 0 is +Z): a yaw of `d` puts it exactly `d` off the crosshair, with no pitch to blur it.
[[nodiscard]] Field make_field() {
    Field f{};
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Enemy, 40, 60},
                                            {UnitKind::Tank, Side::Player, 40, 52},
                                            {UnitKind::Turret, Side::Enemy, 52, 60}}});
    f.driven = f.sim.units()[0].id;
    f.enemy = f.sim.units()[1].id;
    f.friendly = f.sim.units()[2].id;
    f.turret = f.sim.units()[3].id;
    const sim::Unit* tank = f.sim.find(f.enemy);
    f.options.seed = kSeed;
    f.options.extent = kExtent;
    f.options.camera
        = Camera{80.0F, ground_metres(kSeed, tank->x, tank->y) + 0.9F, 80.0F, 0.0F, 0.0F};
    f.options.aim = Aim{640.0F, 360.0F};
    f.options.driven_unit = f.driven;
    return f;
}

// ------------------------------------------------------------------------------ level ground

struct Level {
    int x{};
    int y{};
    bool found{};
};

/// A `side` x `side` block of tiles within forty height units (0.6 tile, 1.25 m) of each other and
/// none a crater: the flattest this world offers, which is not flat -- the solve reads the ground
/// under the muzzle and under the predicted target, so a gentle slope is part of the test.
[[nodiscard]] Level find_level_block(int side) {
    constexpr int kTolerance = 40;
    for (int y = 8; y < sim::kMapTiles - 8 - side; y += 2) {
        for (int x = 8; x < sim::kMapTiles - 8 - side; x += 2) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int j = 0; j <= side && level; ++j) {
                for (int i = 0; i <= side && level; ++i) {
                    const auto t = world::sample(kSeed, x + i, y + j);
                    level = t.terrain != world::Terrain::Crater
                            && std::abs(t.height - h0) <= kTolerance;
                }
            }
            if (level) {
                return Level{x, y, true};
            }
        }
    }
    return Level{};
}

/// A tank of yours at the corner of a level block and a hostile tank `ahead` tiles along +x and
/// `across` along +y, set off at full speed towards (`goal_dx`, `goal_dy`) from where it stands.
struct Duel {
    sim::Sim sim;
    UnitId shooter;
    UnitId target;
    SceneOptions options;
};

[[nodiscard]] Duel make_duel(int ahead, int across, int goal_dx, int goal_dy,
                             UnitKind target_kind = UnitKind::Tank) {
    Duel d{};
    const Level level = find_level_block(16);
    EXPECT_TRUE(level.found);
    const int sx = level.x + 2;
    const int sy = level.y + 8;
    d.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, sx, sy},
                                            {target_kind, Side::Enemy, sx + ahead, sy + across}}});
    d.shooter = d.sim.units()[0].id;
    d.target = d.sim.units()[1].id;
    // The player drives it, from tick 0: a hull under the AI would fire at the target on its own,
    // and the gun would be reloading when the test pulls the trigger.
    d.sim.enqueue(sim::Order{d.shooter, sim::orders::TakeControl{}});
    // A new unit starts with its gun reloading.
    for (int tick = 0; tick < sim::profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        d.sim.step();
    }
    if (goal_dx != 0 || goal_dy != 0) {
        const std::vector<UnitId> roster{d.target};
        for (const sim::Order& order :
             cmd::move_to(roster, sim::Fixed::from_int(sx + ahead + goal_dx),
                          sim::Fixed::from_int(sy + across + goal_dy))) {
            d.sim.enqueue(order);
        }
        for (int tick = 0; tick < 4; ++tick) {
            d.sim.step(); // up to speed: the velocity the dot reads is the one in the unit
        }
    }
    const sim::Unit* gun = d.sim.find(d.shooter);
    d.options.seed = kSeed;
    d.options.extent = kExtent;
    d.options.camera
        = Camera{tiles_of(gun->x) * kMetresPerTile, ground_metres(kSeed, gun->x, gun->y) + 1.4F,
                 tiles_of(gun->y) * kMetresPerTile, 0.0F, 0.0F};
    d.options.camera.yaw = std::atan2(static_cast<float>(ahead) * kMetresPerTile,
                                      static_cast<float>(across) * kMetresPerTile);
    d.options.aim
        = Aim{static_cast<float>(kExtent.width) / 2.0F, static_cast<float>(kExtent.height) / 2.0F};
    d.options.driven_unit = d.shooter;
    return d;
}

/// The order the trigger gives for `weapon` along `barrel` (x, up, z): what `Interaction` sends.
[[nodiscard]] sim::Order trigger_order(Weapon weapon, UnitId shooter, UnitId named,
                                       const std::array<float, 3>& barrel) {
    const sim::Fixed dx = to_fixed(barrel.at(0));
    const sim::Fixed dy = to_fixed(barrel.at(2));
    const sim::Fixed dz = to_fixed(barrel.at(1));
    switch (weapon) {
    case Weapon::Mortar:
        return sim::Order{shooter, sim::orders::FireLob{dx, dy, named, dz}};
    case Weapon::Bouncer:
        return sim::Order{shooter, sim::orders::FireLob{dx, dy, named, dz, sim::Round::Bounce}};
    case Weapon::Shotgun:
        return sim::Order{shooter, sim::orders::FireFan{dx, dy, named, dz}};
    case Weapon::Rocket:
        return sim::Order{shooter, sim::orders::FireRocket{dx, dy, named, dz}};
    case Weapon::Gun:
        break;
    }
    return sim::Order{shooter, sim::orders::FireTowards{dx, dy, named, dz}};
}

/// The kind of fire an order is and its aim, raw: enough to say two trigger pulls are the same one.
[[nodiscard]] std::array<std::int32_t, 5> fire_of(const sim::Order& order) {
    const auto pack = [&order](const auto& fire) {
        return std::array<std::int32_t, 5>{static_cast<std::int32_t>(order.kind()), fire.dir_x.raw,
                                           fire.dir_y.raw, fire.dir_z.raw,
                                           static_cast<std::int32_t>(fire.aimed_at.value)};
    };
    if (const auto* o = std::get_if<sim::orders::FireTowards>(&order.payload)) {
        return pack(*o);
    }
    if (const auto* o = std::get_if<sim::orders::FireLob>(&order.payload)) {
        return pack(*o);
    }
    if (const auto* o = std::get_if<sim::orders::FireFan>(&order.payload)) {
        return pack(*o);
    }
    if (const auto* o = std::get_if<sim::orders::FireRocket>(&order.payload)) {
        return pack(*o);
    }
    return {-1, 0, 0, 0, 0};
}

/// Fire along `barrel`, fly the shell out, and say whether it reached the target: it lost hit
/// points, or -- for the bouncer, which takes no life on contact and only glances off the body --
/// the shell was seen glancing off it.
[[nodiscard]] bool hits(Duel& d, Weapon weapon, const std::array<float, 3>& barrel) {
    const std::uint16_t before = d.sim.find(d.target)->hp;
    d.sim.enqueue(trigger_order(weapon, d.shooter, d.target, barrel));
    d.sim.step();
    bool glanced = false;
    for (int tick = 0; tick < 900 && !d.sim.projectiles().empty(); ++tick) {
        for (const sim::Projectile& shell : d.sim.projectiles()) {
            glanced = glanced || shell.bounced_off == d.target;
        }
        d.sim.step();
    }
    const sim::Unit* target = d.sim.find(d.target);
    return glanced || target == nullptr || !target->alive || target->hp < before;
}

/// The unit vector from the shooter's muzzle to the target's middle where it stands NOW: the aim
/// of a player who leads nothing.
[[nodiscard]] std::array<float, 3> straight_at(const Duel& d) {
    const sim::Unit* gun = d.sim.find(d.shooter);
    const sim::Unit* target = d.sim.find(d.target);
    const float mid
        = static_cast<float>(sim::body_bottom(target->kind).raw + sim::body_top(target->kind).raw)
          * 0.5F;
    const float to_x = tiles_of(target->x) - tiles_of(gun->x);
    const float to_y = tiles_of(target->y) - tiles_of(gun->y);
    const float target_ground
        = static_cast<float>(sim::ground_height(kSeed, target->x, target->y).raw);
    const float gun_ground = static_cast<float>(sim::ground_height(kSeed, gun->x, gun->y).raw);
    const float to_z
        = (target_ground + mid - gun_ground - static_cast<float>(sim::kMuzzleHeight.raw)) / kOne
          / 64.0F;
    const float length = std::sqrt((to_x * to_x) + (to_y * to_y) + (to_z * to_z));
    return {to_x / length, to_z / length, to_y / length};
}

} // namespace

// ------------------------------------------------------------------------------ the cone

TEST(AssistCone, AFewDegreesIsNamedOnceAndIsWiderThanTheCrossAndNarrowerThanTheSky) {
    // The reticle's ticks reach 0.055 of the half-screen at the 30 degree half-FOV.
    const float cross_reach_degrees
        = std::atan(0.055F * std::tan(math::kFovRadians * 0.5F)) / kRadiansPerDegree;
    EXPECT_GT(kAssistConeDegrees, cross_reach_degrees * 2.0F);
    EXPECT_LE(kAssistConeDegrees, 6.0F) << "wider and the dot no longer means 'that one'";
}

TEST(AssistCone, AHostileJustInsideTheConeIsTakenAndJustOutsideIsNot) {
    Field f = make_field();
    f.options.camera.yaw = (kAssistConeDegrees - 0.5F) * kRadiansPerDegree;
    EXPECT_EQ(assist_pick(f.sim, f.options), f.enemy);
    f.options.camera.yaw = -(kAssistConeDegrees - 0.5F) * kRadiansPerDegree;
    EXPECT_EQ(assist_pick(f.sim, f.options), f.enemy) << "and on the other side";
    f.options.camera.yaw = (kAssistConeDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(assist_pick(f.sim, f.options).valid());
    f.options.camera.yaw = -(kAssistConeDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(assist_pick(f.sim, f.options).valid());
}

TEST(AssistCone, OnlyHostilesAreTakenNotAFriendlyInTheWay) {
    Field f = make_field();
    // Our own tank stands between the camera and the enemy, dead on the ray.
    f.options.camera.x = static_cast<float>(f.sim.find(f.friendly)->x.raw) / kOne * kMetresPerTile;
    f.options.camera.z = 80.0F;
    f.options.camera.y
        = ground_metres(kSeed, f.sim.find(f.enemy)->x, f.sim.find(f.enemy)->y) + 0.9F;
    EXPECT_EQ(assist_pick(f.sim, f.options), f.enemy) << "the hostile behind it, not the friend";
    f.options.camera.yaw = 90.0F * kRadiansPerDegree; // looking along +X, at nothing
    EXPECT_FALSE(assist_pick(f.sim, f.options).valid());
}

TEST(AssistCone, TheNearestToTheRayWinsAndAStructureIsAnAssetToo) {
    Field f = make_field();
    // Straight at the turret (a structure): it is the only hostile in the cone.
    const sim::Unit* turret = f.sim.find(f.turret);
    f.options.camera.yaw = std::atan2(tiles_of(turret->x) * kMetresPerTile - 80.0F,
                                      tiles_of(turret->y) * kMetresPerTile - 80.0F);
    f.options.camera.y = ground_metres(kSeed, turret->x, turret->y)
                         + (unit_box_height_metres(turret->kind) * 0.5F);
    EXPECT_EQ(assist_pick(f.sim, f.options), f.turret);
    // A hair towards the tank is nearer the tank's ray than the turret's.
    f.options.camera.yaw = 0.0F;
    f.options.camera.y
        = ground_metres(kSeed, f.sim.find(f.enemy)->x, f.sim.find(f.enemy)->y) + 0.9F;
    EXPECT_EQ(assist_pick(f.sim, f.options), f.enemy);
}

TEST(AssistCone, ATargetOnARidgeFarAboveTheCrosshairIsStillAssistableWhenWithinTheAngle) {
    // The angle is taken in 3D, to the asset's middle: height does not exclude it, only the angle.
    Field f = make_field();
    f.options.camera.y -= 40.0F * std::tan((kAssistConeDegrees - 1.0F) * kRadiansPerDegree);
    EXPECT_EQ(assist_pick(f.sim, f.options), f.enemy) << "just below the middle, inside the cone";
    f.options.camera.y -= 40.0F;
    EXPECT_FALSE(assist_pick(f.sim, f.options).valid()) << "40 m below it, far outside";
}

TEST(AssistCone, WithNoCrosshairOrNothingInTheConeThereIsNothingToTake) {
    Field f = make_field();
    f.options.aim.reset();
    EXPECT_FALSE(assist_pick(f.sim, f.options).valid());
}

// ------------------------------------------------------------------------------ the solve

TEST(AssistShot, AStationaryTargetIsAimedAtWhereItStandsAndSettlesAtOnce) {
    Duel d = make_duel(10, 0, 0, 0);
    const auto shot
        = assist_shot(d.options, Weapon::Gun, *d.sim.find(d.shooter), *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    const std::array<float, 3> straight = straight_at(d);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(shot->barrel.at(axis), straight.at(axis), 0.002F) << axis;
    }
    EXPECT_LE(shot->passes, 2) << "no velocity, no lead, nothing to iterate";
    const sim::Unit* target = d.sim.find(d.target);
    EXPECT_NEAR(shot->aim_metres.at(0), tiles_of(target->x) * kMetresPerTile, 0.01F);
    EXPECT_NEAR(shot->aim_metres.at(2), tiles_of(target->y) * kMetresPerTile, 0.01F);
}

TEST(AssistShot, ACrossingTargetIsLeadAheadOfWhereItIs) {
    Duel d = make_duel(10, -6, 0, 30); // crossing the line of fire, +y
    const auto shot
        = assist_shot(d.options, Weapon::Gun, *d.sim.find(d.shooter), *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    const sim::Unit* target = d.sim.find(d.target);
    const float ahead = shot->aim_metres.at(2) - (tiles_of(target->y) * kMetresPerTile);
    EXPECT_GT(ahead, 4.0F) << "metres the aim point is ahead of the target, in its direction";
    EXPECT_GT(shot->passes, 2);
    EXPECT_LE(shot->passes, kAssistMaxPasses);
    const std::array<float, 3> straight = straight_at(d);
    EXPECT_GT(shot->barrel.at(2) - straight.at(2), 0.0F) << "the barrel is turned towards +y";
}

TEST(AssistShot, TheCrosshairOnTheDotPutsTheViewsForwardOnTheBarrel) {
    Duel d = make_duel(10, -6, 0, 30);
    const auto shot
        = assist_shot(d.options, Weapon::Mortar, *d.sim.find(d.shooter), *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    // Turn the camera to the barrel: yaw 0 looks down +Z, forward = (sin y cos p, sin p, cos y cos
    // p).
    Camera camera = d.options.camera;
    camera.yaw = std::atan2(shot->barrel.at(0), shot->barrel.at(2));
    camera.pitch = std::asin(shot->barrel.at(1));
    const Projected on = project(camera, kExtent, shot->dot_metres.at(0), shot->dot_metres.at(1),
                                 shot->dot_metres.at(2));
    ASSERT_GT(on.depth, 0.0F);
    EXPECT_NEAR(on.x, static_cast<float>(kExtent.width) / 2.0F, 0.5F);
    EXPECT_NEAR(on.y, static_cast<float>(kExtent.height) / 2.0F, 0.5F);
}

TEST(AssistShot, EveryWeaponHitsATargetMovingAcrossItsBowAlongTheBarrelTheDotGives) {
    for (const Weapon weapon : {Weapon::Gun, Weapon::Rocket, Weapon::Mortar, Weapon::Bouncer}) {
        Duel d = make_duel(9, -5, 0, 30);
        const auto shot
            = assist_shot(d.options, weapon, *d.sim.find(d.shooter), *d.sim.find(d.target));
        ASSERT_TRUE(shot.has_value()) << weapon_name(weapon);
        if (!shot.has_value()) {
            return; // for the analyzer, not the test: gtest asserts do not stop it
        }
        EXPECT_TRUE(hits(d, weapon, shot->barrel)) << weapon_name(weapon) << " along the dot";

        // The control: the same shot at where the target IS, leading nothing, misses it.
        Duel c = make_duel(9, -5, 0, 30);
        EXPECT_FALSE(hits(c, weapon, straight_at(c))) << weapon_name(weapon) << " with no lead";
    }
}

TEST(AssistShot, TheShotgunHitsACrossingTargetAtCloseRange) {
    Duel d = make_duel(4, -3, 0, 30);
    const auto shot
        = assist_shot(d.options, Weapon::Shotgun, *d.sim.find(d.shooter), *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    EXPECT_TRUE(hits(d, Weapon::Shotgun, shot->barrel));
}

TEST(AssistShot, ATargetDrivingAwayAndOneDrivingTowardsAreBothLead) {
    for (const int goal : {30, -8}) {
        for (const Weapon weapon : {Weapon::Gun, Weapon::Mortar}) {
            Duel d = make_duel(goal > 0 ? 6 : 12, 0, goal, 0);
            const auto shot
                = assist_shot(d.options, weapon, *d.sim.find(d.shooter), *d.sim.find(d.target));
            ASSERT_TRUE(shot.has_value()) << weapon_name(weapon) << goal;
            if (!shot.has_value()) {
                return; // for the analyzer, not the test: gtest asserts do not stop it
            }
            EXPECT_TRUE(hits(d, weapon, shot->barrel)) << weapon_name(weapon) << " goal " << goal;
        }
    }
}

TEST(AssistShot, ADrivingShootersOwnVelocityIsInTheSolve) {
    Duel d = make_duel(10, -5, 0, 30);
    const std::vector<UnitId> roster{d.shooter};
    // Driving up the range towards the target's side, at the tank's full speed.
    for (const sim::Order& order :
         cmd::move_to(roster, sim::Fixed::from_int(170), sim::Fixed::from_int(32))) {
        d.sim.enqueue(order);
    }
    d.sim.step();
    d.sim.step();
    ASSERT_NE(d.sim.find(d.shooter)->vx.raw, 0) << "the shooter is moving";
    const sim::Unit* gun = d.sim.find(d.shooter);
    d.options.camera.x = tiles_of(gun->x) * kMetresPerTile;
    d.options.camera.z = tiles_of(gun->y) * kMetresPerTile;
    const auto shot = assist_shot(d.options, Weapon::Gun, *gun, *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    EXPECT_TRUE(hits(d, Weapon::Gun, shot->barrel));
}

TEST(AssistShot, ATargetThatChangesCourseAfterTheDotIsDrawnIsMissed) {
    for (const Weapon weapon : {Weapon::Gun, Weapon::Mortar}) {
        Duel d = make_duel(9, -5, 0, 30);
        const auto shot
            = assist_shot(d.options, weapon, *d.sim.find(d.shooter), *d.sim.find(d.target));
        ASSERT_TRUE(shot.has_value());
        if (!shot.has_value()) {
            return; // for the analyzer, not the test: gtest asserts do not stop it
        }
        // The same barrel hits a copy of the duel in which the target holds its course, so the miss
        // below is the turn's and not a bad dot.
        Duel steady = make_duel(9, -5, 0, 30);
        EXPECT_TRUE(hits(steady, weapon, shot->barrel)) << weapon_name(weapon) << " holding course";
        // It turns back the moment the shot is away: not predictable, so it is not hit.
        d.sim.enqueue(trigger_order(weapon, d.shooter, d.target, shot->barrel));
        const std::vector<UnitId> roster{d.target};
        for (const sim::Order& order :
             cmd::move_to(roster, sim::Fixed::from_int(d.sim.find(d.target)->x.raw >> 16),
                          sim::Fixed::from_int(0))) {
            d.sim.enqueue(order);
        }
        const std::uint16_t before = d.sim.find(d.target)->hp;
        for (int tick = 0; tick < 400 && (tick < 2 || !d.sim.projectiles().empty()); ++tick) {
            d.sim.step();
        }
        EXPECT_EQ(d.sim.find(d.target)->hp, before) << weapon_name(weapon);
    }
}

TEST(AssistShot, TheMortarShowsTheLowArcAndAStationaryTargetNeedsLiftAboveTheLine) {
    Duel d = make_duel(10, 0, 0, 0);
    const auto shot
        = assist_shot(d.options, Weapon::Mortar, *d.sim.find(d.shooter), *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    const std::array<float, 3> straight = straight_at(d);
    EXPECT_GT(shot->barrel.at(1), straight.at(1) + 0.05F) << "elevated over the straight line";
    EXPECT_LT(shot->barrel.at(1), std::sin(45.0F * kRadiansPerDegree))
        << "the low arc, not the high";
    EXPECT_TRUE(hits(d, Weapon::Mortar, shot->barrel));
}

TEST(AssistShot, OutOfReachIsNotDrawn) {
    Duel far = make_duel(23, 0, 0, 0); // a tank gun's bolt flies 16 tiles; the mortar's 16 too
    EXPECT_FALSE(
        assist_shot(far.options, Weapon::Gun, *far.sim.find(far.shooter), *far.sim.find(far.target))
            .has_value())
        << "beyond the bolt's travel";
    EXPECT_FALSE(assist_shot(far.options, Weapon::Mortar, *far.sim.find(far.shooter),
                             *far.sim.find(far.target))
                     .has_value())
        << "beyond the mortar's range at any elevation";
}

TEST(AssistShot, ATargetAsFastAsTheShotCannotBeLeadAndIsNotDrawn) {
    // The shotgun's pellet flies at the bolt speed (0.53 tiles a tick); a target given the speed of
    // the shot itself, straight away, is never caught.
    Duel d = make_duel(6, 0, 30, 0);
    sim::Unit runaway = *d.sim.find(d.target);
    runaway.vx = sim::Fixed::from_raw(sim::kProjectileSpeedRaw);
    EXPECT_FALSE(assist_shot(d.options, Weapon::Gun, *d.sim.find(d.shooter), runaway).has_value());
}

// ------------------------------------------------------------------------------ the key

TEST(AssistKey, TTakesTheHostileInTheConeAndOrdersNothingAndSaysNothing) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_EQ(player.assist_target(), f.enemy);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_TRUE(player.message().empty());
    EXPECT_TRUE(player.selection().empty());
    EXPECT_FALSE(player.attack_armed());
    EXPECT_FALSE(player.go_armed());
}

TEST(AssistKey, TWithNothingInTheConeAndNothingHeldDoesNothingAtAll) {
    Field f = make_field();
    f.options.camera.yaw = 90.0F * kRadiansPerDegree;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_FALSE(player.assist_target().valid());
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_TRUE(player.message().empty());
}

TEST(AssistKey, TWithNothingInTheConeReleasesWhatIsHeld) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    ASSERT_EQ(player.assist_target(), f.enemy);
    f.options.camera.yaw = 90.0F * kRadiansPerDegree; // looking away
    player.handle(InputFrame{}, f.sim, f.options);
    EXPECT_EQ(player.assist_target(), f.enemy) << "the lock outlives the crosshair leaving";
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_FALSE(player.assist_target().valid()) << "T on empty air lets go";
    EXPECT_TRUE(player.message().empty());
}

TEST(AssistKey, TWithAnotherHostileInTheConeRetargets) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    ASSERT_EQ(player.assist_target(), f.enemy);
    const sim::Unit* turret = f.sim.find(f.turret);
    f.options.camera.yaw = std::atan2(tiles_of(turret->x) * kMetresPerTile - 80.0F,
                                      tiles_of(turret->y) * kMetresPerTile - 80.0F);
    f.options.camera.y = ground_metres(kSeed, turret->x, turret->y)
                         + (unit_box_height_metres(turret->kind) * 0.5F);
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_EQ(player.assist_target(), f.turret);
}

TEST(AssistKey, TheLockGoesWhenTheTargetIsGone) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    ASSERT_EQ(player.assist_target(), f.enemy);
    const sim::Sim empty
        = sim::Sim::create(sim::Scenario{kSeed, {{UnitKind::Tank, Side::Player, 36, 44}}});
    player.handle(InputFrame{}, empty, f.options);
    EXPECT_FALSE(player.assist_target().valid());
}

TEST(AssistKey, TheShotIsTheBarrelWhetherOrNotATargetIsLatched) {
    // The one rule: the dot draws and never steers. The trigger's orders are identical.
    for (const Weapon weapon : kDrivenWeapons) {
        Field plain = make_field();
        Field assisted = make_field();
        Interaction without{plain.driven};
        Interaction with{assisted.driven};
        with.handle(key_frame(input::kKeyT), assisted.sim, assisted.options);
        ASSERT_TRUE(with.assist_target().valid());
        for (Interaction* player : {&without, &with}) {
            while (player->weapon() != weapon) {
                InputFrame click{};
                click.clicks.push_back(input::Click{0, 0, input::MouseButton::Right, true, false});
                player->handle(click, plain.sim, plain.options);
            }
        }
        InputFrame fire{};
        fire.clicks.push_back(input::Click{640, 360, input::MouseButton::Left, true, false});
        without.handle(fire, plain.sim, plain.options);
        with.handle(fire, assisted.sim, assisted.options);
        const std::vector<sim::Order> a = without.take_orders();
        const std::vector<sim::Order> b = with.take_orders();
        ASSERT_EQ(a.size(), 1U) << weapon_name(weapon);
        ASSERT_EQ(b.size(), 1U) << weapon_name(weapon);
        EXPECT_EQ(fire_of(a.front()), fire_of(b.front())) << weapon_name(weapon);
        EXPECT_NE(fire_of(a.front()).at(0), -1) << "and it is a shot";
    }
}

// ------------------------------------------------------------------------------ the picture

namespace {

struct Seen {
    Duel duel;
    Scene scene;
    std::vector<std::size_t> ink;
};

[[nodiscard]] std::vector<std::size_t> assist_vertices(const Scene& scene) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        if (scene.vertices.at(i).colour == kAssistInk) {
            found.push_back(i);
        }
    }
    return found;
}

[[nodiscard]] float eye_distance(const SceneOptions& options, const SceneVertex& v) {
    const float dx = v.position[0] - options.camera.x;
    const float dy = v.position[1] - options.camera.y;
    const float dz = v.position[2] - options.camera.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

} // namespace

TEST(AssistDraw, NoTargetDrawsNothingAndTheFrameIsAsItWas) {
    Duel d = make_duel(10, -5, 0, 30);
    d.options.weapon = Weapon::Gun;
    const Scene before = build_scene(d.sim, d.options);
    EXPECT_TRUE(assist_vertices(before).empty());
    d.options.assist_target = d.target;
    const Scene after = build_scene(d.sim, d.options);
    EXPECT_GT(after.vertices.size(), before.vertices.size());
    d.options.assist_target = UnitId{};
    EXPECT_EQ(build_scene(d.sim, d.options).vertices.size(), before.vertices.size());
}

TEST(AssistDraw, AThinLineRunsFromTheAssetToASmallDotAtTheAimPoint) {
    Duel d = make_duel(10, -5, 0, 30);
    d.options.weapon = Weapon::Gun;
    d.options.assist_target = d.target;
    const Scene scene = build_scene(d.sim, d.options);
    const std::vector<std::size_t> ink = assist_vertices(scene);
    ASSERT_EQ(ink.size(), 6U + (3U * kAssistDotSlices)) << "one quad and one fan";

    const auto shot
        = assist_shot(d.options, Weapon::Gun, *d.sim.find(d.shooter), *d.sim.find(d.target));
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    const Projected dot = project(d.options.camera, d.options.extent, shot->dot_metres.at(0),
                                  shot->dot_metres.at(1), shot->dot_metres.at(2));
    const auto pixel = [&](std::size_t at) {
        const SceneVertex& v = scene.vertices.at(at);
        return project(d.options.camera, d.options.extent, v.position[0], v.position[1],
                       v.position[2]);
    };
    // The fan's first vertex of every triangle is its centre.
    EXPECT_NEAR(pixel(ink.at(6)).x, dot.x, 0.1F);
    EXPECT_NEAR(pixel(ink.at(6)).y, dot.y, 0.1F);
    // The quad: a, b, c, a, c, d -- the start edge is a and d, the end edge b and c.
    const Projected a = pixel(ink.at(0));
    const Projected dd = pixel(ink.at(5));
    const Projected b = pixel(ink.at(1));
    const Projected c = pixel(ink.at(2));
    const Projected target = attack_point(d.options, *d.sim.find(d.target));
    EXPECT_NEAR((a.x + dd.x) * 0.5F, target.x, 0.1F) << "it starts on the asset";
    EXPECT_NEAR((a.y + dd.y) * 0.5F, target.y, 0.1F);
    EXPECT_NEAR((b.x + c.x) * 0.5F, dot.x, 0.1F) << "and ends on the dot";
    EXPECT_NEAR((b.y + c.y) * 0.5F, dot.y, 0.1F);
    // Thin: a couple of pixels. Small: a few pixels across.
    EXPECT_LT(std::hypot(a.x - dd.x, a.y - dd.y), 3.0F);
    const Projected rim = pixel(ink.at(7));
    EXPECT_LT(std::hypot(rim.x - dot.x, rim.y - dot.y), 6.0F);
    EXPECT_GT(std::hypot(rim.x - dot.x, rim.y - dot.y), 1.0F);
}

TEST(AssistDraw, TheDotIsNearerThanTheLineAndFartherThanTheReticle) {
    // Equal-distance overlays lose the depth test, so the layers are distinct and pinned.
    Duel d = make_duel(10, -5, 0, 30);
    d.options.weapon = Weapon::Gun;
    d.options.assist_target = d.target;
    const Scene scene = build_scene(d.sim, d.options);
    const std::vector<std::size_t> ink = assist_vertices(scene);
    ASSERT_FALSE(ink.empty());
    const float line = eye_distance(d.options, scene.vertices.at(ink.at(0)));
    const float dot = eye_distance(d.options, scene.vertices.at(ink.at(6)));
    EXPECT_LT(dot, line);
    EXPECT_GT(dot, 1.5F) << "behind the reticle's 1.5 m, so the cross is drawn over the dot";
}

TEST(AssistDraw, ATurretWearsTheHealthBarWhileAssistedAndNotOtherwise) {
    // A structure never wore a floating bar. The assisted one does, from the existing bar, so
    // the shot at a Guardian shows its health. A shooter with no solution isolates the bar.
    Duel d = make_duel(20, 0, 0, 0, UnitKind::Turret); // 20 tiles: beyond the gun's 16-tile bolt
    d.options.viewer_unit = d.shooter;
    d.options.viewer_side = Side::Player;
    d.options.weapon = Weapon::Gun;
    // Look at it.
    const sim::Unit* turret = d.sim.find(d.target);
    d.options.camera.yaw = std::atan2(tiles_of(turret->x) * kMetresPerTile - d.options.camera.x,
                                      tiles_of(turret->y) * kMetresPerTile - d.options.camera.z);
    const std::size_t plain = build_scene(d.sim, d.options).vertices.size();
    d.options.assist_target = d.target;
    const Scene assisted = build_scene(d.sim, d.options);
    EXPECT_TRUE(assist_vertices(assisted).empty()) << "out of reach: no line, no dot";
    EXPECT_EQ(assisted.vertices.size() - plain, kUnitBarVertexCount) << "exactly one bar";
}

} // namespace protekt::game
