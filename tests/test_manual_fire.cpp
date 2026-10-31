#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <variant>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/player.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

/// M2: the wide pitch, the wheel, and the mortar fired by hand.
namespace protekt::game {
namespace {

using input::Click;
using input::InputFrame;
using input::MouseButton;
using sim::Order;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};
constexpr float kHalfPi = 1.5707963F;

struct Level {
    int x{};
    int y{};
    bool found{};
};

/// A row of `length` + 1 tiles within six height units of each other, none a crater, and the row
/// beside it likewise: ground on which the ideal ladder is not tangled with a hill.
[[nodiscard]] Level find_level_row(int length) {
    constexpr int kTolerance = 6;
    for (int y = 5; y < sim::kMapTiles - 5; ++y) {
        for (int x = 5; x + length < sim::kMapTiles - 5; ++x) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                const auto t = world::sample(kSeed, x + i, y);
                const auto below = world::sample(kSeed, x + i, y + 1);
                level = t.terrain != world::Terrain::Crater && std::abs(t.height - h0) <= kTolerance
                        && std::abs(below.height - h0) <= kTolerance;
            }
            if (level) {
                return Level{x, y, true};
            }
        }
    }
    return Level{};
}

[[nodiscard]] float as_float(sim::Fixed value) {
    return static_cast<float>(value.raw) / static_cast<float>(sim::Fixed::kOne);
}

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank{};
    sim::UnitId enemy{};
    Level row;
};

/// The driven tank on the head of a level row, an enemy tank far off the shot's line.
[[nodiscard]] Rig make_rig() {
    Rig rig{};
    rig.row = find_level_row(16);
    EXPECT_TRUE(rig.row.found);
    const int far_y = rig.row.y < 100 ? rig.row.y + 40 : rig.row.y - 40;
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Tank, Side::Player, rig.row.x, rig.row.y},
                                              {UnitKind::Tank, Side::Enemy, rig.row.x, far_y}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        (unit.side == Side::Player ? rig.tank : rig.enemy) = unit.id;
    }
    // A new unit starts with its gun reloading.
    for (int tick = 0; tick < sim::profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        rig.sim.step();
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.aim
        = Aim{static_cast<float>(kExtent.width) / 2.0F, static_cast<float>(kExtent.height) / 2.0F};
    return rig;
}

/// The camera where the game puts it: over the driven tank, looking along (yaw, pitch).
void look(Rig& rig, float yaw, float pitch) {
    const sim::Unit* gun = rig.sim.find(rig.tank);
    rig.options.camera = Camera{as_float(gun->x) * kMetresPerTile,
                                ground_metres(kSeed, gun->x, gun->y) + kRideHeightMetres + 3.0F,
                                as_float(gun->y) * kMetresPerTile, yaw, pitch};
}

[[nodiscard]] InputFrame trigger() {
    InputFrame frame{};
    frame.clicks.push_back(Click{640, 360, MouseButton::Left, true, false});
    return frame;
}

/// Select `weapon` the way a player does now: one RIGHT CLICK per step of the wrap (2026-10-23).
void switch_to(Rig& rig, Interaction& player, Weapon weapon) {
    for (int step = 0; step < 8 && player.weapon() != weapon; ++step) {
        InputFrame click{};
        click.clicks.push_back(input::Click{0, 0, input::MouseButton::Right, true, false});
        player.handle(click, rig.sim, rig.options);
    }
    ASSERT_EQ(player.weapon(), weapon);
}

[[nodiscard]] InputFrame wheel(int notches) {
    InputFrame frame{};
    frame.wheel = notches;
    return frame;
}

/// Pull the trigger with the barrel along +x at `pitch`, enqueue what it ordered, step the sim
/// until no shell is left, and return how many tiles the shell got along +x from the muzzle.
struct Flight {
    std::vector<Order> orders;
    float tiles{};
    sim::Projectile first{};
    bool launched{};
};

[[nodiscard]] Flight fire_along_x(Rig& rig, Weapon weapon, float pitch) {
    Flight flight;
    Interaction player{rig.tank};
    switch_to(rig, player, weapon);
    look(rig, kHalfPi, pitch);
    player.handle(trigger(), rig.sim, rig.options);
    flight.orders = player.take_orders();
    for (const Order& order : flight.orders) {
        rig.sim.enqueue(order);
    }
    const float start = as_float(rig.sim.find(rig.tank)->x);
    float last = start;
    rig.sim.step();
    flight.launched = !rig.sim.projectiles().empty();
    if (flight.launched) {
        flight.first = rig.sim.projectiles().front();
    }
    for (int tick = 0; tick < 400 && !rig.sim.projectiles().empty(); ++tick) {
        last = as_float(rig.sim.projectiles().front().x);
        rig.sim.step();
    }
    flight.tiles = last - start;
    return flight;
}

// --- the pitch limit ---------------------------------------------------------------------------

TEST(PitchLimit, IsExactlyOnePointThreeRadians) { EXPECT_EQ(kLookPitchLimit, 1.3F); }

TEST(PitchLimit, LookingUpClampsAtItAndLookingDownAtItsNegative) {
    PlayerCommander player;
    InputState input{};
    input.look_pitch = 100.0F;
    player.set_input(input);
    std::vector<Order> out;
    player.think(sim::UnitId{1}, sim::Observation{}, out);
    EXPECT_EQ(player.view_pitch(), 1.3F);
    input.look_pitch = -100.0F;
    player.set_input(input);
    player.think(sim::UnitId{1}, sim::Observation{}, out);
    EXPECT_EQ(player.view_pitch(), -1.3F);
}

TEST(PitchLimit, TheLadderReachesTheMortarsWholeRangeAtTheLimit) {
    const float range = as_float(sim::spec_of(UnitKind::MortarBike).range);
    EXPECT_GE(mortar_landing_tiles(kMortarRungPitches.back()), range - 0.2F);
    EXPECT_LE(kMortarRungPitches.back(), kLookPitchLimit);
    EXPECT_EQ(mortar_rung(kLookPitchLimit), kMortarRungCount - 1);
}

// --- the wheel (RETIRED 2026-10-23: the right button changes weapons, the wheel does not) -------

[[nodiscard]] Weapon after_wheel(Weapon start, int notches) {
    Rig rig = make_rig();
    Interaction player{rig.tank};
    // Walk to `start` the way a player does -- a right click -- BOUNDED, so a switch that stops
    // working fails the assertion instead of hanging the suite.
    for (int step = 0; step < 8 && player.weapon() != start; ++step) {
        InputFrame click{};
        click.clicks.push_back(input::Click{0, 0, input::MouseButton::Right, true, false});
        player.handle(click, rig.sim, rig.options);
    }
    EXPECT_EQ(player.weapon(), start) << "the right click still walks the wrap";
    player.handle(wheel(notches), rig.sim, rig.options);
    return player.weapon();
}

TEST(Wheel, NoLongerSwitchesTheWeaponInEitherDirection) {
    for (const Weapon start : kDrivenWeapons) {
        EXPECT_EQ(after_wheel(start, -1), start) << "the wheel is not a weapon control any more";
        EXPECT_EQ(after_wheel(start, +1), start);
        EXPECT_EQ(after_wheel(start, -2), start) << "a flick is not a step per notch either";
        EXPECT_EQ(after_wheel(start, +3), start);
    }
}

TEST(Wheel, OneNotchIsStillOneNotchOfNothing) {
    // The frame still carries the wheel (the window reports it); the game has no use for it now, so
    // zero notches, one notch and many notches are all the same weapon.
    EXPECT_EQ(after_wheel(Weapon::Mortar, 0), Weapon::Mortar);
    EXPECT_EQ(after_wheel(Weapon::Mortar, 5), Weapon::Mortar);
    EXPECT_EQ(after_wheel(Weapon::Bouncer, -5), Weapon::Bouncer);
}

TEST(Wheel, ASeparateFrameIsStillNoNotchWorthOfSwitch) {
    // The notch used to be read one frame at a time (`ASeparateFrameIsASeparateNotch`): now a frame
    // of wheel, however many of them, leaves the weapon where it was and the trigger as it was.
    Rig rig = make_rig();
    Interaction player{rig.tank};
    for (int frame = 0; frame < 3; ++frame) {
        player.handle(wheel(-1), rig.sim, rig.options);
        player.handle(trigger(), rig.sim, rig.options);
        static_cast<void>(player.take_orders()); // the trigger still fires; the wheel does not pick
    }
    EXPECT_EQ(player.weapon(), Weapon::Gun) << "three frames of a notch each switched nothing";
}

// --- manual fire -------------------------------------------------------------------------------

TEST(ManualFire, TheMortarOrdersALobAlongTheBarrelNotAFireTowards) {
    Rig rig = make_rig();
    const Flight flight = fire_along_x(rig, Weapon::Mortar, 0.6F);
    ASSERT_EQ(flight.orders.size(), 1U);
    EXPECT_EQ(flight.orders.front().kind(), sim::OrderKind::FireLob);
}

TEST(ManualFire, TheShellLeavesWithTheLaddersOwnClimbAndTheMortarsOwnSpeed) {
    // The no-lying-sight proof, exactly: the shell's velocity is the one `mortar_landing_tiles`
    // flies for that pitch -- both go through sim::mortar_climb.
    const sim::UnitSpec& mortar = sim::spec_of(UnitKind::MortarBike);
    for (const float pitch : kMortarRungPitches) {
        Rig rig = make_rig();
        const Flight flight = fire_along_x(rig, Weapon::Mortar, pitch);
        ASSERT_TRUE(flight.launched) << pitch;
        const std::int64_t rise = std::lround(static_cast<double>(std::tan(pitch)) * 65536.0);
        // Quantising the barrel to Q16.16 may move the ratio by a hair; one raw unit of the
        // direction is a few raw units of climb.
        const std::int64_t want = sim::mortar_climb(sim::Fixed::kOne, rise);
        // `first` is read after the launch tick's step, which has already lost one tick of gravity.
        EXPECT_NEAR(static_cast<double>(flight.first.vz.raw) + mortar.gravity.raw,
                    static_cast<double>(want),
                    static_cast<double>(mortar.bolt_speed.raw) * 64.0 / 65536.0 * 8.0 + 8.0)
            << "pitch " << pitch;
        EXPECT_NEAR(static_cast<double>(flight.first.vx.raw),
                    static_cast<double>(mortar.bolt_speed.raw), 8.0);
        EXPECT_EQ(flight.first.gravity.raw, mortar.gravity.raw);
        EXPECT_EQ(flight.first.splash_radius.raw, mortar.splash_radius.raw);
        EXPECT_EQ(flight.first.max_travel.raw, mortar.max_travel.raw);
    }
}

TEST(ManualFire, TheShellLandsWhereTheLadderSaysOnLevelGround) {
    for (const float pitch : kMortarRungPitches) {
        Rig rig = make_rig();
        const Flight flight = fire_along_x(rig, Weapon::Mortar, pitch);
        ASSERT_TRUE(flight.launched) << pitch;
        const float ladder = mortar_landing_tiles(pitch);
        const float step = as_float(sim::spec_of(UnitKind::MortarBike).bolt_speed);
        // The last step the shell took before it landed brackets the touchdown, and the row is
        // level to within six height units (the same slack M1's ladder test gives).
        EXPECT_GE(ladder, flight.tiles - 0.25F) << "pitch " << pitch;
        EXPECT_LE(ladder, flight.tiles + step + 0.25F) << "pitch " << pitch;
    }
}

TEST(ManualFire, TheLimitPitchLandsAtTheMortarsFullRange) {
    Rig rig = make_rig();
    const Flight flight = fire_along_x(rig, Weapon::Mortar, kLookPitchLimit);
    ASSERT_TRUE(flight.launched);
    EXPECT_GE(flight.tiles, as_float(sim::spec_of(UnitKind::MortarBike).range) - 0.5F);
}

TEST(ManualFire, FiresWithNothingUnderTheCrosshairAndDoesNotRefuse) {
    Rig rig = make_rig();
    const Flight flight = fire_along_x(rig, Weapon::Mortar, 0.8F);
    ASSERT_EQ(flight.orders.size(), 1U);
    EXPECT_TRUE(flight.launched) << "an empty crosshair is still a shot";
    EXPECT_GT(flight.tiles, 1.0F);
}

TEST(ManualFire, DoesNotLeadOrAimAtAMovingEnemy) {
    // Same barrel, same seed, with and without an enemy driving across the line of fire: the shell
    // flies the barrel, so the touchdown is the same to the raw unit.
    Rig calm = make_rig();
    const Flight a = fire_along_x(calm, Weapon::Mortar, 0.8F);

    Rig busy = make_rig();
    const sim::Unit* gun = busy.sim.find(busy.tank);
    // The enemy is ordered to cross in front of the muzzle while the shell is in the air.
    busy.sim.enqueue(Order{
        busy.enemy, sim::orders::MoveTo{sim::Fixed::from_raw(gun->x.raw + 5 * 65536), gun->y}});
    const Flight b = fire_along_x(busy, Weapon::Mortar, 0.8F);
    EXPECT_EQ(a.first.vx.raw, b.first.vx.raw);
    EXPECT_EQ(a.first.vy.raw, b.first.vy.raw);
    EXPECT_EQ(a.first.vz.raw, b.first.vz.raw);
    EXPECT_EQ(a.tiles, b.tiles);
}

TEST(ManualFire, ATargetUnderTheMarkDoesNotBendTheShell) {
    // The mark is on the enemy, the barrel is pointed elsewhere: the order names the enemy as
    // intent and the shell still flies the barrel.
    Rig rig = make_rig();
    Interaction player{rig.tank};
    switch_to(rig, player, Weapon::Mortar);
    look(rig, kHalfPi, 0.5F);
    const sim::Unit* enemy = rig.sim.find(rig.enemy);
    const Projected p = project(rig.options.camera, kExtent, as_float(enemy->x) * kMetresPerTile,
                                ground_metres(kSeed, enemy->x, enemy->y) + 0.9F,
                                as_float(enemy->y) * kMetresPerTile);
    rig.options.aim = Aim{p.x, p.y};
    player.handle(InputFrame{[] {
                      InputFrame f{};
                      f.clicks.push_back(Click{0, 0, MouseButton::Left, true, false});
                      return f;
                  }()},
                  rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    ASSERT_EQ(orders.front().kind(), sim::OrderKind::FireLob);
    const auto& lob = std::get<sim::orders::FireLob>(orders.front().payload);
    const auto barrel = math::basis_from(kHalfPi, 0.5F).forward;
    EXPECT_NEAR(as_float(lob.dir_x), barrel.at(0), 1e-3F);
    EXPECT_NEAR(as_float(lob.dir_z), barrel.at(1), 1e-3F);
}

TEST(ManualFire, TheGunKeepsTheAutoAssistedShotItAlwaysWas) {
    // The gun is the HULL's weapon: one FireTowards along the barrel, flown as the tank's own
    // straight single-target bolt. This half of the old gun-and-rocket contract is unchanged.
    Rig rig = make_rig();
    const Flight flight = fire_along_x(rig, Weapon::Gun, 0.6F);
    ASSERT_EQ(flight.orders.size(), 1U);
    EXPECT_EQ(flight.orders.front().kind(), sim::OrderKind::FireTowards);
    ASSERT_TRUE(flight.launched);
    EXPECT_EQ(flight.first.damage, sim::spec_of(UnitKind::Tank).damage);
    EXPECT_EQ(flight.first.gravity.raw, 0);
    EXPECT_EQ(flight.first.splash_radius.raw, 0);
}

TEST(ManualFire, TheRocketIsItsOwnWeaponWithItsOwnReachNotTheHulls) {
    // B, and Harri's ask (2026-10-27: "can the rocket have more reach?"). This REPLACES the old
    // contract that the weapon choice "changes nothing the sim sees": the trigger now sends
    // `FireRocket`, whose numbers come from `kRocketSpec`, so the rocket owns its damage, its
    // reload and its distance. It is still aimed on the gun's own barrel -- the crosshairs agree --
    // but it is a different shot, and the sim can see the difference.
    Rig gun_rig = make_rig();
    Rig rocket_rig = make_rig();
    const Flight gun = fire_along_x(gun_rig, Weapon::Gun, 0.6F);
    const Flight rocket = fire_along_x(rocket_rig, Weapon::Rocket, 0.6F);

    ASSERT_EQ(rocket.orders.size(), 1U);
    EXPECT_EQ(rocket.orders.front().kind(), sim::OrderKind::FireRocket);
    ASSERT_TRUE(rocket.launched);
    EXPECT_EQ(rocket.first.damage, sim::kRocketSpec.damage)
        << "the weapon's damage, not the hull's";
    EXPECT_GT(rocket.first.max_travel.raw, gun.first.max_travel.raw)
        << "the rocket's bolt is allowed to fly farther than the gun's";
    EXPECT_EQ(rocket.first.gravity.raw, 0);
    EXPECT_EQ(rocket.first.splash_radius.raw, 0) << "one bolt, single target";

    // The same barrel as the gun's: only the round differs.
    EXPECT_EQ(std::get<sim::orders::FireRocket>(rocket.orders.front().payload).dir_x,
              std::get<sim::orders::FireTowards>(gun.orders.front().payload).dir_x);
    EXPECT_NE(gun_rig.sim.state_hash(), rocket_rig.sim.state_hash())
        << "the weapon choice is a different shot now, not a cosmetic reticle";
}

TEST(ManualFire, TheGunTheShotgunAndTheRocketFlyTheBarrelNotTheTarget) {
    // The mark is ON the enemy and the barrel is a little off it: each weapon names the enemy as
    // intent and carries the view's own direction, pitch included, not the line to the enemy.
    for (const Weapon weapon : {Weapon::Gun, Weapon::Shotgun, Weapon::Rocket}) {
        Rig rig = make_rig();
        rig.options.radius_tiles
            = 100; // the enemy stands 40 tiles out: draw it, so it can be picked
        Interaction player{rig.tank};
        switch_to(rig, player, weapon);
        look(rig, 0.0F, 0.0F);
        const sim::Unit* enemy = rig.sim.find(rig.enemy);
        const float to_x = as_float(enemy->x) * kMetresPerTile - rig.options.camera.x;
        const float to_z = as_float(enemy->y) * kMetresPerTile - rig.options.camera.z;
        const float to_y = ground_metres(kSeed, enemy->x, enemy->y) + 0.9F - rig.options.camera.y;
        // Look at the enemy, then turn the barrel a few hundredths of a radian off it: the enemy is
        // still on the screen under the mark, and the barrel and the line to it now differ.
        const float yaw = std::atan2(to_x, to_z) + 0.05F;
        const float pitch = std::atan2(to_y, std::hypot(to_x, to_z)) + 0.05F;
        look(rig, yaw, pitch);
        const Projected p = project(
            rig.options.camera, kExtent, as_float(enemy->x) * kMetresPerTile,
            ground_metres(kSeed, enemy->x, enemy->y) + 0.9F, as_float(enemy->y) * kMetresPerTile);
        rig.options.aim = Aim{p.x, p.y};
        ASSERT_TRUE(pick(rig.sim, rig.options, p.x, p.y).has_unit())
            << "the mark really is on the enemy: " << static_cast<int>(weapon);
        player.handle(InputFrame{[] {
                          InputFrame f{};
                          f.clicks.push_back(Click{0, 0, MouseButton::Left, true, false});
                          return f;
                      }()},
                      rig.sim, rig.options);
        const std::vector<Order> orders = player.take_orders();
        ASSERT_EQ(orders.size(), 1U);
        sim::Fixed x{};
        sim::Fixed y{};
        sim::Fixed up{};
        sim::UnitId named{};
        if (weapon == Weapon::Gun) {
            const auto& fire = std::get<sim::orders::FireTowards>(orders.front().payload);
            x = fire.dir_x;
            y = fire.dir_y;
            up = fire.dir_z;
            named = fire.aimed_at;
        } else if (weapon == Weapon::Shotgun) {
            const auto& fire = std::get<sim::orders::FireFan>(orders.front().payload);
            x = fire.dir_x;
            y = fire.dir_y;
            up = fire.dir_z;
            named = fire.aimed_at;
        } else {
            const auto& fire = std::get<sim::orders::FireRocket>(orders.front().payload);
            x = fire.dir_x;
            y = fire.dir_y;
            up = fire.dir_z;
            named = fire.aimed_at;
        }
        const auto barrel = math::basis_from(yaw, pitch).forward;
        EXPECT_EQ(named, rig.enemy) << static_cast<int>(weapon);
        EXPECT_NEAR(as_float(x), barrel.at(0), 1e-3F) << static_cast<int>(weapon);
        EXPECT_NEAR(as_float(up), barrel.at(1), 1e-3F) << static_cast<int>(weapon);
        EXPECT_NEAR(as_float(y), barrel.at(2), 1e-3F) << static_cast<int>(weapon);
        // The barrel is not the line to the enemy, or this proves nothing.
        const float line = std::sqrt((to_x * to_x) + (to_y * to_y) + (to_z * to_z));
        const float toward
            = ((to_x * as_float(x)) + (to_y * as_float(up)) + (to_z * as_float(y))) / line;
        EXPECT_LT(toward, 0.999F) << static_cast<int>(weapon);
    }
}

TEST(ManualFire, TheMortarShellGoesThroughTheSameProjectilePathAsAnyShot) {
    // Hashed state: a lob in the air changes the hash, and fires one Shot like any manual shot.
    Rig rig = make_rig();
    const std::uint64_t before = rig.sim.state_hash();
    Interaction player{rig.tank};
    switch_to(rig, player, Weapon::Mortar);
    look(rig, kHalfPi, 0.6F);
    player.handle(trigger(), rig.sim, rig.options);
    for (const Order& order : player.take_orders()) {
        rig.sim.enqueue(order);
    }
    rig.sim.step();
    ASSERT_EQ(rig.sim.projectiles().size(), 1U);
    EXPECT_NE(rig.sim.state_hash(), before);
    EXPECT_EQ(rig.sim.shots().size(), 1U);
    EXPECT_EQ(rig.sim.find(rig.tank)->cooldown + 1U, sim::spec_of(UnitKind::MortarBike).cooldown)
        << "the mortar reloads like a mortar";
}

TEST(ManualFire, TwoSameSeedRunsAreByteIdentical) {
    auto run = [] {
        Rig rig = make_rig();
        std::vector<std::uint64_t> hashes;
        Interaction player{rig.tank};
        switch_to(rig, player, Weapon::Mortar);
        look(rig, kHalfPi, 1.1F);
        for (int tick = 0; tick < 120; ++tick) {
            if (tick % 50 == 0) {
                player.handle(trigger(), rig.sim, rig.options);
                for (const Order& order : player.take_orders()) {
                    rig.sim.enqueue(order);
                }
            }
            rig.sim.step();
            hashes.push_back(rig.sim.state_hash());
        }
        return hashes;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
} // namespace protekt::game
