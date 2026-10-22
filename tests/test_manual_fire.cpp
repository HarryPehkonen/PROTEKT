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
    const int notches = weapon == Weapon::Gun ? 0 : (weapon == Weapon::Rocket ? 1 : 2);
    player.handle(wheel(-notches), rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), weapon);
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
    const float range = as_float(sim::spec_of(UnitKind::Mortar).range);
    EXPECT_GE(mortar_landing_tiles(kMortarRungPitches.back()), range - 0.2F);
    EXPECT_LE(kMortarRungPitches.back(), kLookPitchLimit);
    EXPECT_EQ(mortar_rung(kLookPitchLimit), kMortarRungCount - 1);
}

// --- the wheel ---------------------------------------------------------------------------------

[[nodiscard]] Weapon after_wheel(Weapon start, int notches) {
    Rig rig = make_rig();
    Interaction player{rig.tank};
    // Walk to `start` the way a player does: by the same key Q.
    while (player.weapon() != start) {
        InputFrame q{};
        q.keys.push_back(input::KeyEvent{input::kKeyQ, true, false, false, false});
        player.handle(q, rig.sim, rig.options);
    }
    player.handle(wheel(notches), rig.sim, rig.options);
    return player.weapon();
}

TEST(Wheel, DownCyclesGunRocketMortarAndUpReverses) {
    EXPECT_EQ(after_wheel(Weapon::Gun, -1), Weapon::Rocket);
    EXPECT_EQ(after_wheel(Weapon::Rocket, -1), Weapon::Mortar);
    EXPECT_EQ(after_wheel(Weapon::Mortar, +1), Weapon::Rocket);
    EXPECT_EQ(after_wheel(Weapon::Rocket, +1), Weapon::Gun);
}

TEST(Wheel, WrapsTheWayQDoes) {
    EXPECT_EQ(after_wheel(Weapon::Mortar, -1), next_weapon(Weapon::Mortar));
    EXPECT_EQ(after_wheel(Weapon::Mortar, -1), Weapon::Gun);
    EXPECT_EQ(after_wheel(Weapon::Gun, +1), previous_weapon(Weapon::Gun));
    EXPECT_EQ(after_wheel(Weapon::Gun, +1), Weapon::Mortar);
}

TEST(Wheel, OneNotchIsExactlyOneStepAndAFlickIsOneStepPerNotch) {
    for (const Weapon start : kDrivenWeapons) {
        EXPECT_EQ(after_wheel(start, -1), next_weapon(start)) << "one notch, one step";
        EXPECT_EQ(after_wheel(start, +1), previous_weapon(start));
        EXPECT_EQ(after_wheel(start, 0), start);
        EXPECT_EQ(after_wheel(start, -2), next_weapon(next_weapon(start)))
            << "a flick of two notches is two steps, not three or one";
    }
}

TEST(Wheel, ASeparateFrameIsASeparateNotch) {
    Rig rig = make_rig();
    Interaction player{rig.tank};
    player.handle(wheel(-1), rig.sim, rig.options);
    player.handle(InputFrame{}, rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Rocket) << "a frame with no notch changes nothing";
    player.handle(wheel(-1), rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Mortar);
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
    const sim::UnitSpec& mortar = sim::spec_of(UnitKind::Mortar);
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
        const float step = as_float(sim::spec_of(UnitKind::Mortar).bolt_speed);
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
    EXPECT_GE(flight.tiles, as_float(sim::spec_of(UnitKind::Mortar).range) - 0.5F);
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
    player.handle(wheel(-2), rig.sim, rig.options);
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

TEST(ManualFire, GunAndRocketKeepTheAutoAssistedShotAsEverTheyWere) {
    // Today's trigger is one FireTowards along shot_direction; the gun and the rocket still make
    // exactly that, whatever the pitch, and fly it as a straight bolt of the tank's own.
    for (const Weapon weapon : {Weapon::Gun, Weapon::Rocket}) {
        Rig rig = make_rig();
        const Flight flight = fire_along_x(rig, weapon, 0.6F);
        ASSERT_EQ(flight.orders.size(), 1U);
        EXPECT_EQ(flight.orders.front().kind(), sim::OrderKind::FireTowards);
        ASSERT_TRUE(flight.launched);
        EXPECT_EQ(flight.first.gravity.raw, 0);
        EXPECT_EQ(flight.first.splash_radius.raw, 0);
    }
    Rig gun = make_rig();
    Rig rocket = make_rig();
    const Flight g = fire_along_x(gun, Weapon::Gun, 0.6F);
    const Flight r = fire_along_x(rocket, Weapon::Rocket, 0.6F);
    EXPECT_EQ(std::get<sim::orders::FireTowards>(g.orders.front().payload).dir_x,
              std::get<sim::orders::FireTowards>(r.orders.front().payload).dir_x);
    EXPECT_EQ(gun.sim.state_hash(), rocket.sim.state_hash())
        << "the weapon choice changes nothing the sim sees for gun and rocket";
}

TEST(ManualFire, TheMortarShellGoesThroughTheSameProjectilePathAsAnyShot) {
    // Hashed state: a lob in the air changes the hash, and fires one Shot like any manual shot.
    Rig rig = make_rig();
    const std::uint64_t before = rig.sim.state_hash();
    Interaction player{rig.tank};
    player.handle(wheel(-2), rig.sim, rig.options);
    look(rig, kHalfPi, 0.6F);
    player.handle(trigger(), rig.sim, rig.options);
    for (const Order& order : player.take_orders()) {
        rig.sim.enqueue(order);
    }
    rig.sim.step();
    ASSERT_EQ(rig.sim.projectiles().size(), 1U);
    EXPECT_NE(rig.sim.state_hash(), before);
    EXPECT_EQ(rig.sim.shots().size(), 1U);
    EXPECT_EQ(rig.sim.find(rig.tank)->cooldown + 1U, sim::spec_of(UnitKind::Mortar).cooldown)
        << "the mortar reloads like a mortar";
}

TEST(ManualFire, TwoSameSeedRunsAreByteIdentical) {
    auto run = [] {
        Rig rig = make_rig();
        std::vector<std::uint64_t> hashes;
        Interaction player{rig.tank};
        player.handle(wheel(-2), rig.sim, rig.options);
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
