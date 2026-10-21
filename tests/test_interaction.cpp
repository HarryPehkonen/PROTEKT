#include "protekt/game/interaction.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <variant>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/sounds.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::Click;
using input::InputFrame;
using input::KeyEvent;
using input::MouseButton;
using sim::Order;
using sim::OrderKind;
using sim::Scenario;
using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank_a{}; ///< driven: at tile (40,40)
    sim::UnitId tank_b{}; ///< at tile (42,40)
    sim::UnitId turret{}; ///< at tile (40,42)
    sim::UnitId enemy{};  ///< at tile (38,44)
    sim::UnitId enemy2{}; ///< at tile (46,44): a second target, clear of every other unit's line
};

/// A known layout, so a click test knows which pixel belongs to which unit.
///
/// NONE of these may share the camera's x: the camera sits at tile (40,30) looking down +Z, so a
/// unit at x=40 is directly behind whatever is in front of it on that line, and a click on it lands
/// on the near unit instead. The first version of this rig put the turret and the enemy at x=40
/// behind the driven tank, and five tests failed for exactly that reason -- the picker was right.
[[nodiscard]] Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(Scenario{kSeed,
                                        {{UnitKind::Tank, Side::Player, 40, 40},
                                         {UnitKind::Tank, Side::Player, 44, 41},
                                         {UnitKind::Turret, Side::Player, 36, 41},
                                         {UnitKind::Tank, Side::Enemy, 38, 44},
                                         {UnitKind::Tank, Side::Enemy, 46, 44}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.kind == UnitKind::Tank && unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.kind == UnitKind::Tank && unit.side == Side::Player) {
            rig.tank_b = unit.id;
        } else if (unit.kind == UnitKind::Turret) {
            rig.turret = unit.id;
        } else if (!rig.enemy.valid()) {
            rig.enemy = unit.id;
        } else {
            rig.enemy2 = unit.id;
        }
    }
    // Place the camera from tank A's tile, so the world position is derived the same way the mesh
    // and the picker derive it.
    const float ground = height_metres(kSeed, 40, 40);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, ground + 3.0F, 60.0F, 0.0F, -0.12F};
    return rig;
}

/// The rig's mark; a rig without one has it at the origin, which no test here relies on.
[[nodiscard]] Aim mark_of(const Rig& rig) {
    return rig.options.aim.has_value() ? *rig.options.aim : Aim{};
}

/// A click with the MARK on `unit`.
///
/// Under the aim model a click does not aim: the mark aims, and it is the middle of the screen with
/// the mouse's height. So this moves the mark onto the unit and records a click as well; the claims
/// the tests below make are unchanged (this click selects that unit), and the aim rule itself has
/// its own tests in tests/test_aim.cpp.
[[nodiscard]] Click at(Rig& rig, sim::UnitId unit, MouseButton button = MouseButton::Left,
                       bool shift = false) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
    return Click{static_cast<int>(p.x), static_cast<int>(p.y), button, true, shift};
}

/// The mark on a pixel that is definitely ground: low on the screen, well below every unit.
[[nodiscard]] Click ground_click(Rig& rig, bool right = false, bool shift = false) {
    rig.options.aim = Aim{640.0F, 700.0F};
    return Click{640, 700, right ? MouseButton::Right : MouseButton::Left, true, shift};
}

[[nodiscard]] InputFrame with_click(const Click& click) {
    InputFrame frame{};
    frame.clicks.push_back(click);
    return frame;
}

[[nodiscard]] InputFrame with_key(std::int32_t scancode, bool shift = false) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, shift});
    return frame;
}

/// Select with the keyboard, which is the only way to select now: the mark on `unit`, then Space.
void choose(Rig& rig, Interaction& player, sim::UnitId unit, bool shift = false) {
    static_cast<void>(at(rig, unit));
    player.handle(with_key(input::kKeySpace, shift), rig.sim, rig.options);
}

/// A new gun is still reloading; step until the shot a click orders will actually be made.
void reload(sim::Sim& sim) {
    for (int tick = 0; tick < sim::profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        sim.step();
    }
}

/// Drive `unit` and keep it selected: Space cannot select the tank being driven, so it is selected
/// first, then swapped into with X. Whoever was driven is an ordinary unit afterwards.
void select_and_drive(Rig& rig, Interaction& player, sim::UnitId unit) {
    choose(rig, player, unit);
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    ASSERT_EQ(player.driven(), unit);
}

[[nodiscard]] std::vector<sim::UnitId> selected(const Interaction& interaction) {
    std::vector<sim::UnitId> ids;
    for (const sim::UnitId id : interaction.selection().units()) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace

// ---------------------------------------------------------------- selection

TEST(Interaction, ShiftSpaceStillAddsAndRemoves) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
    choose(rig, player, rig.turret, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}));
    choose(rig, player, rig.tank_b, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}));
}

// ---------------------------------------------------------------- the trigger

TEST(Trigger, ALeftClickOnAnOwnUnitNamesItAndDoesNotChangeTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.turret);
    const std::vector<sim::UnitId> before = selected(player);

    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).kind(), OrderKind::FireTowards);
    EXPECT_EQ(orders.at(0).unit, rig.tank_a);
    EXPECT_EQ(std::get<sim::orders::FireTowards>(orders.at(0).payload).aimed_at, rig.tank_b);
    EXPECT_EQ(selected(player), before) << "a click is a trigger, not a selection";
}

TEST(Trigger, ALeftClickOnAnUnselectedOwnUnitDoesNotSelectIt) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    player.handle(with_click(at(rig, rig.turret, MouseButton::Left, true)), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty()) << "Shift does not turn the trigger into a selector";
}

TEST(Trigger, AClickOnGroundOrSkyFiresAnUnaimedBoltAndLeavesTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);

    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    {
        const std::vector<Order> orders = player.take_orders();
        ASSERT_EQ(orders.size(), 1U) << "ground";
        EXPECT_EQ(orders.at(0).kind(), OrderKind::FireTowards) << "ground";
    }
    EXPECT_EQ(selected(player), before);

    for (sim::Tick t = 0; t < kManualShotIntervalTicks; ++t) {
        rig.sim.step();
    }
    rig.options.aim = Aim{640.0F, 5.0F}; // the top of the frame: sky
    player.handle(with_click(Click{640, 5, MouseButton::Left, true, false}), rig.sim, rig.options);
    {
        const std::vector<Order> orders = player.take_orders();
        ASSERT_EQ(orders.size(), 1U) << "sky";
        EXPECT_EQ(orders.at(0).kind(), OrderKind::FireTowards) << "sky";
    }
    EXPECT_EQ(selected(player), before);
}

TEST(Trigger, AClickOnTheGroundFiresABoltOntoTheOrderQueue) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);

    // Both halves, and neither may be traded for the other: the bolt is seen AND heard.
    const bool heard = player.take_shot_voice();
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).kind(), OrderKind::FireTowards);
    EXPECT_EQ(orders.at(0).unit, rig.tank_a) << "the unit the player is sitting in fires";
    EXPECT_TRUE(heard) << "a shot at nothing is still a shot to hear";
}

TEST(Trigger, AClickOnAUnitFiresFromTheMuzzleAtThePointTheMarkSees) {
    // The crosshair chooses a TRAJECTORY, and a unit under it is intent -- what the shot is for.
    // The trajectory is the muzzle's line through the point the mark's ray reaches (the unit's
    // box), the same as `shot_direction` says; `aimed_at` is the unit and the order is the same
    // kind as a shot at the ground.
    for (const float yaw : {0.0F, 0.1F, -0.1F}) {
        Rig rig = make_rig();
        rig.options.camera.yaw = yaw;
        Interaction on_ground{rig.tank_a};
        on_ground.handle(with_click(ground_click(rig)), rig.sim, rig.options);
        const std::vector<Order> ground = on_ground.take_orders();
        ASSERT_EQ(ground.size(), 1U) << yaw;
        const auto& along = std::get<sim::orders::FireTowards>(ground.at(0).payload);
        EXPECT_FALSE(along.aimed_at.valid()) << "a shot at the ground names no one";

        Interaction on_unit{rig.tank_a};
        on_unit.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
        const std::vector<Order> unit = on_unit.take_orders();
        ASSERT_EQ(unit.size(), 1U) << yaw;
        const auto& at_enemy = std::get<sim::orders::FireTowards>(unit.at(0).payload);
        EXPECT_EQ(at_enemy.aimed_at, rig.enemy) << yaw;

        const Pick seen = pick(rig.sim, rig.options, mark_of(rig).x, mark_of(rig).y);
        ASSERT_TRUE(seen.has_unit()) << yaw;
        const std::array<float, 3> want
            = shot_direction(*rig.sim.find(rig.tank_a), rig.options, seen, mark_of(rig));
        const float scale = static_cast<float>(sim::Fixed::kOne);
        EXPECT_NEAR(static_cast<float>(at_enemy.dir_x.raw) / scale, want.at(0), 1e-3F) << yaw;
        EXPECT_NEAR(static_cast<float>(at_enemy.dir_y.raw) / scale, want.at(2), 1e-3F) << yaw;
        EXPECT_NEAR(static_cast<float>(at_enemy.dir_z.raw) / scale, want.at(1), 1e-3F) << yaw;
    }
}

TEST(Trigger, TheTriggerStillMakesOneBoltAndOneSoundPerClick) {
    for (const bool on_unit : {false, true}) {
        Rig rig = make_rig();
        Interaction player{rig.tank_a};
        const Click click = on_unit ? at(rig, rig.enemy) : ground_click(rig);
        player.handle(with_click(click), rig.sim, rig.options);
        EXPECT_TRUE(player.take_shot_voice())
            << "the app announces every manual shot, on a unit or not";
        EXPECT_FALSE(player.take_shot_voice()) << "once";
        const std::vector<Order> orders = player.take_orders();
        ASSERT_EQ(orders.size(), 1U) << on_unit;
        EXPECT_EQ(orders.at(0).kind(), OrderKind::FireTowards);

        // The order into a sim whose gun is ready and has nothing in range to fire at on its own,
        // so the only bolt there can be is the one the click ordered.
        sim::Sim lone = sim::Sim::create(Scenario{kSeed, {{UnitKind::Tank, Side::Player, 20, 60}}});
        reload(lone);
        lone.enqueue(Order{lone.units().at(0).id, orders.at(0).payload});
        lone.step();
        EXPECT_EQ(lone.projectiles().size(), 1U) << "one click, one bolt";
    }
}

TEST(Trigger, AManualShotAtAHostileInRangeEmitsNoFiredEvent) {
    // THE SOUND REGRESSION. `aimed_at` can now be valid on a manual shot, and a valid sim Shot
    // target is exactly what the observer turns into a Fired event -- a second voice for a shot the
    // app already announces. The sim must record the manual shot with NO target, whatever it names.
    //
    // The click names the hostile, and the app voices it:
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
    ASSERT_TRUE(player.take_shot_voice()) << "the app's own sound is what the player hears";
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    ASSERT_EQ(std::get<sim::orders::FireTowards>(orders.at(0).payload).aimed_at, rig.enemy);

    // ... and the sim, given a manual shot that names a hostile which really is in range this tick
    // (a gun that has one fires on its own the moment it reloads, so the tank advances into range
    // on the same tick), reports no Fired for it. The control: the sim's own gun fire is reported.
    const auto fired_by_tank = [](bool manual) {
        sim::Sim field = sim::Sim::create(Scenario{kSeed,
                                                   {{UnitKind::Tank, Side::Player, 100, 100},
                                                    {UnitKind::Factory, Side::Enemy, 112, 101}}});
        const sim::UnitId tank = field.units().at(0).id;
        const sim::UnitId hostile = field.units().at(1).id;
        reload(field);
        const sim::Unit& target = *field.find(hostile);
        field.enqueue(Order{tank, sim::orders::MoveTo{target.x, target.y}});
        if (manual) {
            field.enqueue(Order{tank, sim::orders::FireTowards{sim::Fixed::from_int(1),
                                                               sim::Fixed::zero(), hostile}});
        }
        const sim::UnitSnapshot before = field.units();
        const sim::Tick tick = field.tick();
        field.step();
        if (manual) {
            EXPECT_EQ(field.shots().size(), 1U) << "the manual shot really was made";
            EXPECT_FALSE(field.shots().front().target.valid())
                << "and names no one to the observer";
        }
        int fired = 0;
        for (const sim::BattleEvent& e : sim::events_between(before, field, tick)) {
            fired += e.kind == sim::EventKind::Fired && e.actor == tank ? 1 : 0;
        }
        return fired;
    };
    EXPECT_EQ(fired_by_tank(true), 0) << "the player would hear this shot twice";
    EXPECT_EQ(fired_by_tank(false), 1) << "control: gun fire the sim decided on IS reported";
}

TEST(Trigger, AShotAtAUnitIsTheAppsToAnnounceNotTheSims) {
    // The inverse of what this once pinned: the click on a unit used to be a FocusFire that the
    // sim voiced through Fired. Now it is a manual shot, and the app's own sound is the only one.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_shot_voice());
}

namespace {

[[nodiscard]] float as_float(sim::Fixed value) {
    return static_cast<float>(value.raw) / static_cast<float>(sim::Fixed::kOne);
}

/// Put the camera where the game does -- over the driven tank, 3 m above its hull -- looking along
/// (yaw, pitch), with the mark in the middle of the screen, which is where the reticle is.
void eye_over(Rig& rig, sim::UnitId tank, float yaw, float pitch) {
    const sim::Unit* gun = rig.sim.find(tank);
    rig.options.camera = Camera{as_float(gun->x) * kMetresPerTile,
                                ground_metres(kSeed, gun->x, gun->y) + kRideHeightMetres + 3.0F,
                                as_float(gun->y) * kMetresPerTile, yaw, pitch};
    rig.options.aim
        = Aim{static_cast<float>(kExtent.width) / 2.0F, static_cast<float>(kExtent.height) / 2.0F};
}

/// The order a click makes with the camera looking along (yaw, pitch).
[[nodiscard]] sim::orders::FireTowards shot_looking(Rig& rig, float yaw, float pitch) {
    eye_over(rig, rig.tank_a, yaw, pitch);
    Interaction player{rig.tank_a};
    player.handle(with_click(Click{640, 360, MouseButton::Left, true, false}), rig.sim,
                  rig.options);
    const std::vector<Order> orders = player.take_orders();
    EXPECT_EQ(orders.size(), 1U);
    return std::get<sim::orders::FireTowards>(orders.at(0).payload);
}

} // namespace

TEST(Trigger, AShotGoesWhereTheCameraLooksWithTheCamerasOwnSigns) {
    // THE SIGNS. The camera's forward is whatever `math::basis_from` says it is. The camera sits
    // over the gun, so aiming the shot from the muzzle at the point the mark sees moves it only
    // in the vertical: the HEADING is the camera's, exactly, in all four quadrants -- a flipped
    // sign on either map axis fails at least one yaw. The shot is never aimed LOWER than the look
    // (the muzzle is below the eye, so it climbs to meet the mark's ray), and a look well above or
    // below the horizon keeps its own sign. (The order is (map x, map y, up) = (forward.x,
    // forward.z, forward.y).)
    for (const float yaw : {0.0F, 0.7F, 1.5707964F, 2.4F, 3.1415927F, -0.7F, -1.5707964F, -2.4F}) {
        for (const float pitch : {-0.9F, -0.4F, 0.0F, 0.3F, 0.9F}) {
            Rig rig = make_rig();
            const sim::orders::FireTowards fire = shot_looking(rig, yaw, pitch);
            const float x = as_float(fire.dir_x);
            const float y = as_float(fire.dir_y);
            const float up = as_float(fire.dir_z);
            const auto forward = math::basis_from(yaw, pitch).forward;
            EXPECT_NEAR(x * forward.at(2) - y * forward.at(0), 0.0F, 2e-3F)
                << "the heading is the camera's: " << yaw << " " << pitch;
            EXPECT_GT((x * forward.at(0)) + (y * forward.at(2)), 0.0F) << yaw << " " << pitch;
            EXPECT_GE(up, forward.at(1) - 1e-3F)
                << "never aimed below the look: " << yaw << " " << pitch;
            const float length = std::sqrt((x * x) + (y * y) + (up * up));
            EXPECT_NEAR(length, 1.0F, 2e-3F) << "the direction is a unit vector";
        }
    }
    Rig up = make_rig();
    EXPECT_GT(shot_looking(up, 0.0F, 0.4F).dir_z.raw, 0) << "looking UP must aim up";
    Rig down = make_rig();
    EXPECT_LT(shot_looking(down, 0.0F, -0.4F).dir_z.raw, 0) << "looking DOWN must aim down";
}

namespace {

/// THE MEASUREMENT: fire from the middle of the screen, take the bolt's line, find where it
/// crosses the depth of what the mark sees, and project that point through the renderer's own
/// projection. Returns the distance in pixels from the mark; `depth` gets that depth in metres.
/// The reticle is the camera's ray through the mark; the bolt leaves the MUZZLE, below the eye.
[[nodiscard]] float landing_miss_px(Rig& rig, sim::UnitId tank, float& depth) {
    const Aim mark = mark_of(rig);
    Interaction player{tank};
    player.handle(with_click(Click{static_cast<int>(mark.x), static_cast<int>(mark.y),
                                   MouseButton::Left, true, false}),
                  rig.sim, rig.options);
    for (const Order& order : player.take_orders()) {
        rig.sim.enqueue(order);
    }
    rig.sim.step();
    const auto bolt_of_player = [&]() {
        for (const sim::Projectile& p : rig.sim.projectiles()) {
            if (p.shooter == tank) {
                return p;
            }
        }
        ADD_FAILURE() << "no bolt from the player";
        return sim::Projectile{};
    };
    const sim::Projectile first = bolt_of_player();
    rig.sim.step();
    const sim::Projectile later = bolt_of_player();

    // The bolt's line in world metres: a point on it and its direction (x, up, z).
    const std::array<float, 3> p0{as_float(first.x) * kMetresPerTile,
                                  as_float(first.z) * kMetresPerHeightUnit,
                                  as_float(first.y) * kMetresPerTile};
    const std::array<float, 3> v{(as_float(later.x) - as_float(first.x)) * kMetresPerTile,
                                 (as_float(later.z) - as_float(first.z)) * kMetresPerHeightUnit,
                                 (as_float(later.y) - as_float(first.y)) * kMetresPerTile};
    const Camera& eye = rig.options.camera;
    const auto forward = math::basis_from(eye.yaw, eye.pitch).forward;
    const auto depth_of = [&](float x, float y, float z) {
        return ((x - eye.x) * forward.at(0)) + ((y - eye.y) * forward.at(1))
               + ((z - eye.z) * forward.at(2));
    };
    // The depth, along the camera's forward, of what the mark sees.
    const Pick seen = pick(rig.sim, rig.options, mark.x, mark.y);
    const RayPoint target
        = ray_point(eye, kExtent, mark.x, mark.y, seen.distance_tiles * kMetresPerTile);
    depth = depth_of(target.x, target.y, target.z);
    const float along
        = (depth - depth_of(p0[0], p0[1], p0[2]))
          / ((v[0] * forward.at(0)) + (v[1] * forward.at(1)) + (v[2] * forward.at(2)));
    const Projected landed = project(eye, kExtent, p0[0] + (v[0] * along), p0[1] + (v[1] * along),
                                     p0[2] + (v[2] * along));
    return std::hypot(landed.x - mark.x, landed.y - mark.y);
}

} // namespace

TEST(Trigger, TheBoltFliesWhereTheCameraLooksOnTheScreenItself) {
    // The same claim end to end and through the renderer's own projection, for a level, an upward
    // and a downward look at ground or sky: the bolt crosses the middle of the screen -- where the
    // mark is -- at the depth the mark sees. A sign error anywhere between the camera and the sim
    // puts it above or below the mark. A lone tank, so nothing else fires: the one bolt in the
    // air is the player's.
    for (const float pitch : {0.0F, 0.35F, -0.35F}) {
        for (const float yaw : {0.0F, 0.5F, 1.0F}) {
            Rig rig = make_rig();
            rig.sim = sim::Sim::create(Scenario{kSeed, {{UnitKind::Tank, Side::Player, 40, 40}}});
            const sim::UnitId tank{1};
            reload(rig.sim);
            eye_over(rig, tank, yaw, pitch);
            float depth = 0.0F;
            EXPECT_LT(landing_miss_px(rig, tank, depth), 2.0F)
                << "yaw " << yaw << " pitch " << pitch;
        }
    }
}

TEST(Parallax, TheBoltLandsOnTheMarkAtOneHundredMetres) {
    // THE DIVERGENCE, at the range the player reported it. Fired along the camera's own direction
    // the bolt runs parallel to the mark and passes under it by the muzzle's drop: 13.2 px at
    // 98.7 m before the shot was aimed from the muzzle at the point the mark sees. Now it must
    // land on the mark.
    Rig rig = make_rig();
    rig.sim = sim::Sim::create(Scenario{
        kSeed, {{UnitKind::Tank, Side::Player, 40, 40}, {UnitKind::Tank, Side::Enemy, 40, 90}}});
    const sim::UnitId tank{1};
    const sim::Unit* target = rig.sim.find(sim::UnitId{2});
    ASSERT_NE(target, nullptr);
    reload(rig.sim);
    // Look at the enemy, as the player does: pitch the camera until the middle of the screen is on
    // the enemy's body.
    eye_over(rig, tank, 0.0F, 0.0F);
    const float to_x = as_float(target->x) * kMetresPerTile - rig.options.camera.x;
    const float to_z = as_float(target->y) * kMetresPerTile - rig.options.camera.z;
    const float to_y = ground_metres(kSeed, target->x, target->y) + 0.9F - rig.options.camera.y;
    rig.options.camera.yaw = std::atan2(to_x, to_z);
    rig.options.camera.pitch = std::atan2(to_y, std::hypot(to_x, to_z));
    ASSERT_TRUE(pick(rig.sim, rig.options, mark_of(rig).x, mark_of(rig).y).has_unit());
    float depth = 0.0F;
    const float miss_px = landing_miss_px(rig, tank, depth);
    EXPECT_GT(depth, 95.0F) << "the target must be about 100 m out for this to be the 100 m case";
    std::cout << "[parallax] bolt lands " << miss_px << " px from the mark at " << depth << " m\n";
    EXPECT_LT(miss_px, 2.0F) << "the shot must land where the reticle is drawn";
}

TEST(Trigger, AClickOnGroundOrSkyIsAShotThePlayerCanHear) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    EXPECT_FALSE(player.take_shot_voice()) << "no click, no shot";
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_shot_voice()) << "no order must not mean no sound";
    EXPECT_FALSE(player.take_shot_voice()) << "drained once";

    const audio::Voice voice = shot_voice();
    EXPECT_EQ(voice.kind, audio::SoundKind::YourShot);
    EXPECT_FLOAT_EQ(voice.gain, 1.0F);
    EXPECT_FLOAT_EQ(voice.pan, 0.0F);
}

TEST(Trigger, TwoClicksInsideOneIntervalAreOneShotAndALaterClickIsAnother) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    InputFrame two{};
    two.clicks.push_back(ground_click(rig));
    two.clicks.push_back(ground_click(rig));
    player.handle(two, rig.sim, rig.options);
    EXPECT_TRUE(player.take_shot_voice());
    EXPECT_FALSE(player.take_shot_voice()) << "the second click in the same frame is swallowed";

    for (sim::Tick t = 0; t + 1 < kManualShotIntervalTicks; ++t) {
        rig.sim.step();
    }
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_FALSE(player.take_shot_voice()) << "one tick short of the interval";

    rig.sim.step();
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_shot_voice()) << "a full interval later the gun is ready";
}

TEST(Trigger, TheTimerLimitsTheButtonForOrdersToo) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    InputFrame two{};
    two.clicks.push_back(at(rig, rig.enemy));
    two.clicks.push_back(at(rig, rig.enemy));
    player.handle(two, rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
}

TEST(Trigger, TheTimerDoesNotTouchOtherInput) {
    // A right click or a key right after a shot is not a shot.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    static_cast<void>(player.take_shot_voice());
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
}

TEST(Interaction, LeftClickOnAHostileNamesItAndKeepsTheSelection) {
    // The assertion is about the ORDER, and deliberately not about the message: a test that checks
    // "something was refused" keeps passing when the click stops being a refusal.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "clicking the enemy must not throw away the player's own selection";

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1u) << "the trigger orders one unit and nobody else";
    EXPECT_EQ(orders.at(0).kind(), sim::OrderKind::FireTowards)
        << "it must not be an Attack: that would set a hunting stance and fight the keyboard";
    EXPECT_EQ(orders.at(0).unit, rig.tank_a)
        << "the unit the player is sitting in is the one firing";
    EXPECT_EQ(std::get<sim::orders::FireTowards>(orders.at(0).payload).aimed_at, rig.enemy);
    EXPECT_FALSE(player.message().empty()) << "the player has to be told what happened";
}

TEST(Interaction, TabClearsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.turret, true);
    ASSERT_EQ(player.selection().size(), 2U);
    player.handle(with_key(input::kKeyTab), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
}

// ---------------------------------------------------------------- space and T

TEST(Interaction, SpaceTogglesTheUnitUnderTheCrosshairWithOrWithoutShift) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeySpace, true), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}));

    player.handle(with_key(input::kKeySpace, true), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "shift-space on a selected unit toggles it out";

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}))
        << "plain space adds, as shift-space does";
    EXPECT_TRUE(player.message().empty());
}

TEST(Interaction, PlainSpaceOnAnUnselectedOwnTankAddsItToTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.turret);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}))
        << "no shift needed to build a group";
}

TEST(Interaction, PlainSpaceOnASelectedTankRemovesIt) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.turret);
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}));
}

TEST(Interaction, TwoSpacesOnTheSameTankAreNetNothing) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.turret);
    const std::vector<sim::UnitId> before = selected(player);
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), before);
}

TEST(Interaction, SpaceOnTheDrivenTankLeavesTheSelectionAlone) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);
    choose(rig, player, rig.tank_a);
    choose(rig, player, rig.tank_a, true);
    EXPECT_EQ(selected(player), before);
    EXPECT_TRUE(player.message().empty());
}

TEST(Interaction, SpaceOverGroundHostileOrNothingLeavesTheSelectionAlone) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);
    ASSERT_EQ(before.size(), 1U);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "ground";
    EXPECT_TRUE(player.message().empty()) << "ignored, not refused";

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "hostile";
    EXPECT_TRUE(player.message().empty());

    rig.options.aim.reset();
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "no aim point";
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Interaction, TOrdersAnAttackFromEverySelectedUnitAndKeepsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    select_and_drive(rig, player, rig.tank_b);
    choose(rig, player, rig.turret);
    choose(rig, player, rig.tank_a);
    const std::vector<sim::UnitId> before = selected(player);
    ASSERT_EQ(before.size(), 3U);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U) << "the driven tank is not ordered";
    EXPECT_LT(orders.at(0).unit, orders.at(1).unit) << "ascending id order";
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind(), OrderKind::Attack);
        EXPECT_NE(order.unit, rig.tank_b);
        EXPECT_EQ(std::get<sim::orders::Attack>(order.payload).target, rig.enemy);
    }
    EXPECT_EQ(selected(player), before);
}

TEST(Interaction, TWithNothingSelectedIsRefusedAndSaysWhy) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "Nothing is selected to give an order to.");
}

TEST(Interaction, TOffAHostileIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "ground";
    EXPECT_FALSE(player.message().empty());

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "own unit";
    EXPECT_FALSE(player.message().empty());
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
}

TEST(Interaction, TWithOnlyTheDrivenTankSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    select_and_drive(rig, player, rig.tank_b);
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "You are driving that one; select another unit to give orders.");
}

TEST(Interaction, TRetargetsWithoutReselecting) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    const std::vector<Order> first = player.take_orders();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(std::get<sim::orders::Attack>(first.front().payload).target, rig.enemy);

    static_cast<void>(at(rig, rig.enemy2));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    const std::vector<Order> second = player.take_orders();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second.front().unit, rig.tank_b);
    EXPECT_EQ(std::get<sim::orders::Attack>(second.front().payload).target, rig.enemy2);
    EXPECT_EQ(selected(player), before);
}

TEST(Interaction, EscapeClearsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    ASSERT_FALSE(player.selection().empty());
    player.handle(with_key(input::kKeyEscape), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
}

// ---------------------------------------------------------------- orders

TEST(Interaction, RightClickOnTheGroundMovesTheSelectionThere) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), OrderKind::MoveTo);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
    EXPECT_GT(std::get<sim::orders::MoveTo>(orders.front().payload).y.raw, 0)
        << "the order carries the tile the click landed on";
}

TEST(MoveKey, MOrdersEverySelectedUnitToTheGroundUnderTheMark) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.turret);
    rig.options.aim = Aim{640.0F, 700.0F};
    const Pick ground = pick(rig.sim, rig.options, 640.0F, 700.0F);
    ASSERT_TRUE(ground.has_tile());

    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U);
    for (const Order& order : orders) {
        ASSERT_EQ(order.kind(), OrderKind::MoveTo);
        const auto& move = std::get<sim::orders::MoveTo>(order.payload);
        EXPECT_EQ(move.x, sim::Fixed::from_int(ground.tile_x));
        EXPECT_EQ(move.y, sim::Fixed::from_int(ground.tile_y));
    }
    EXPECT_EQ(orders.at(0).unit, rig.tank_b);
    EXPECT_EQ(orders.at(1).unit, rig.turret);
}

TEST(MoveKey, MWithNothingSelectedIssuesNoOrders) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    rig.options.aim = Aim{640.0F, 700.0F};
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(MoveKey, MWithOnlyTheDrivenTankSelectedIssuesNoOrders) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_a);
    rig.options.aim = Aim{640.0F, 700.0F};
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(MoveKey, MWithTheMarkOverAUnitStillOrdersTheGroundNotTheUnit) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    static_cast<void>(at(rig, rig.enemy)); // the mark on a hostile
    const Aim aim = mark_of(rig);
    const Pick picked = pick(rig.sim, rig.options, aim.x, aim.y);
    ASSERT_TRUE(picked.has_unit());
    ASSERT_TRUE(picked.has_tile());

    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    ASSERT_EQ(orders.front().kind(), OrderKind::MoveTo) << "not an Attack";
    const auto& move = std::get<sim::orders::MoveTo>(orders.front().payload);
    EXPECT_EQ(move.x, sim::Fixed::from_int(picked.tile_x));
    EXPECT_EQ(move.y, sim::Fixed::from_int(picked.tile_y));
}

TEST(MoveKey, MWithNoMarkIssuesNoOrders) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    rig.options.aim.reset();
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Interaction, TheDrivenTankIsNeverOrderedByAClick) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    select_and_drive(rig, player, rig.tank_b);
    choose(rig, player, rig.tank_a);
    ASSERT_EQ(player.selection().size(), 2U);

    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "only the unit the player is not sitting in may be ordered";
    EXPECT_EQ(orders.front().unit, rig.tank_a);
}

TEST(Interaction, RightClickWithOnlyTheDrivenTankSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_a);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty())
        << "the player must be told that he is driving that one, not left guessing";
}

TEST(Interaction, RightClickOnAHostileOrdersAnAttack) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(at(rig, rig.enemy, MouseButton::Right)), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), OrderKind::Attack);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
    EXPECT_EQ(std::get<sim::orders::Attack>(orders.front().payload).target, rig.enemy);
}

TEST(Interaction, RightClickWithNothingSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty());
}

TEST(Interaction, OrdersAreDrainedOnce) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
    EXPECT_TRUE(player.take_orders().empty()) << "the same click must not be applied twice";
}

TEST(Interaction, ADeadUnitLeavesTheSelection) {
    // Its own scenario: the turret must actually DIE, so it is surrounded by three enemy tanks. The
    // shared rig cannot promise that, and a test whose precondition might not happen proves
    // nothing.
    sim::Sim sim = sim::Sim::create(Scenario{kSeed,
                                             {{UnitKind::Turret, Side::Player, 40, 42},
                                              {UnitKind::Tank, Side::Enemy, 40, 43},
                                              {UnitKind::Tank, Side::Enemy, 41, 42},
                                              {UnitKind::Tank, Side::Enemy, 39, 42}}});
    sim::UnitId turret{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::Turret) {
            turret = unit.id;
        }
    }
    ASSERT_TRUE(turret.valid());
    const float ground = height_metres(kSeed, 40, 42);
    SceneOptions options{};
    options.seed = kSeed;
    options.extent = kExtent;
    options.camera = Camera{80.0F, ground + 6.0F, 70.0F, 0.0F, -0.3F};

    Interaction player{sim::UnitId{}};
    const sim::Unit* t = sim.find(turret);
    const Projected p
        = project(options.camera, kExtent, static_cast<float>(t->x.raw) / 65536.0F * kMetresPerTile,
                  ground + 0.9F, static_cast<float>(t->y.raw) / 65536.0F * kMetresPerTile);
    options.aim = Aim{p.x, p.y}; // the MARK selects, so the mark goes on the turret
    const InputFrame frame = with_key(input::kKeySpace);
    player.handle(frame, sim, options);
    ASSERT_EQ(selected(player), (std::vector<sim::UnitId>{turret}));

    std::uint32_t ticks = 0;
    while (ticks < 6000U && sim.find(turret)->alive) {
        sim.step();
        ++ticks;
    }
    ASSERT_FALSE(sim.find(turret)->alive) << "the turret never died, so nothing is proved";
    player.handle(InputFrame{}, sim, options);
    EXPECT_TRUE(player.selection().empty()) << "a dead unit cannot stay selected";
}

TEST(Interaction, TheSameInputProducesTheSameOrders) {
    const auto play = [] {
        Rig rig = make_rig();
        Interaction player{rig.tank_a};
        choose(rig, player, rig.tank_b);
        choose(rig, player, rig.turret, true);
        player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
        return player.take_orders();
    };
    const std::vector<Order> first = play();
    const std::vector<Order> second = play();
    ASSERT_EQ(first.size(), second.size());
    ASSERT_EQ(first.size(), 2U) << "both selected units get the order, in ascending id order";
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first.at(i).unit, second.at(i).unit);
        EXPECT_EQ(first.at(i).kind(), second.at(i).kind());
        EXPECT_EQ(std::get<sim::orders::MoveTo>(first.at(i).payload).x.raw,
                  std::get<sim::orders::MoveTo>(second.at(i).payload).x.raw);
        EXPECT_EQ(std::get<sim::orders::MoveTo>(first.at(i).payload).y.raw,
                  std::get<sim::orders::MoveTo>(second.at(i).payload).y.raw);
    }
}

TEST(Interaction, AClickAimsWithTheMarkAndNotItsOwnPixel) {
    // The mark is the gun. A click's own pixel is only where the pointer happened to be: it decides
    // nothing, or the mark could say one thing while the click did another.
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.enemy)); // puts the MARK on the enemy
    ASSERT_TRUE(rig.options.aim.has_value());
    const Click elsewhere{5, 700, MouseButton::Left, true, false}; // no unit is behind that pixel
    player.handle(with_click(elsewhere), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(std::get<sim::orders::FireTowards>(orders.at(0).payload).aimed_at, rig.enemy);
}

TEST(Interaction, AClickWithNoMarkDoesNothing) {
    // No pointer has ever been seen, so there is no mark and nothing a click could mean. This is
    // what keeps a headless or scripted frame from selecting anything by accident.
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    ASSERT_FALSE(rig.options.aim.has_value());
    const Click click{640, 360, MouseButton::Left, true, false};
    player.handle(with_click(click), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.take_shot_voice()) << "and it makes no sound either";
}

// ---------------------------------------------------------------- X swaps tanks

TEST(Swap, XOnAnOwnTankUnderTheCrosshairDrivesIt) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_b);
    EXPECT_FALSE(player.message().empty()) << "success says what happened";
}

TEST(Swap, AnEnemyTankIsRefusedWithAReason) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_a);
    EXPECT_EQ(player.message(), "That tank is not yours -- you can only swap into your own.");
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Swap, AnOwnTurretIsRefusedWithAReason) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_a);
    EXPECT_EQ(player.message(), "That is not a tank -- you can only swap into a tank.");
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Swap, NothingUnderTheCrosshairIsRefusedWithAReason) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_a);
    EXPECT_EQ(player.message(), "There is nothing under the crosshair to swap into.");
}

TEST(Swap, TheTankAlreadyDrivenIsRefusedWithAReason) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.tank_a));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_a);
    EXPECT_EQ(player.message(), "You are already driving that tank.");
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Swap, TheVacatedTankIsToldToStopAndEndsWithZeroVelocity) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    // Get the old tank rolling, as the driver's MoveTo would have.
    rig.sim.enqueue(
        Order{rig.tank_a, sim::orders::MoveTo{sim::Fixed::from_int(40), sim::Fixed::from_int(55)}});
    for (int i = 0; i < 20; ++i) {
        rig.sim.step();
    }
    const sim::Unit* before = rig.sim.find(rig.tank_a);
    ASSERT_TRUE(before->vx.raw != 0 || before->vy.raw != 0) << "the rig must have it moving";

    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.tank_a);
    EXPECT_EQ(orders.at(0).kind(), OrderKind::Stop);

    for (const Order& order : orders) {
        rig.sim.enqueue(order);
    }
    rig.sim.step();
    const sim::Unit* after = rig.sim.find(rig.tank_a);
    EXPECT_EQ(after->vx.raw, 0);
    EXPECT_EQ(after->vy.raw, 0);
}

TEST(Swap, AfterASwapTheNewTankIsNotOrderableAndTheOldOneIs) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    select_and_drive(rig, player, rig.tank_b);
    choose(rig, player, rig.tank_a); // the vacated tank is selectable again
    ASSERT_EQ(player.selection().size(), 2U);
    static_cast<void>(player.take_orders());

    static_cast<void>(ground_click(rig, true));
    player.handle(with_click(Click{640, 700, MouseButton::Right, true, false}), rig.sim,
                  rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "only the vacated tank takes the order";
    EXPECT_EQ(orders.at(0).unit, rig.tank_a);
}

// ---------------------------------------------------------------- the retreat -> return loop

constexpr std::int32_t kHomeX = 10;
constexpr std::int32_t kHomeY = 12;

/// What the player's side sees: `tank` at `hp`, and (unless `factory` is false) a living factory.
/// Ids are the rig's; the factory borrows the turret's, because only the kind matters to the loop.
[[nodiscard]] sim::Observation seen(const Rig& rig, sim::UnitId tank, std::uint16_t hp,
                                    bool factory = true) {
    sim::Observation observation{};
    sim::UnitView view{};
    view.id = tank;
    view.kind = UnitKind::Tank;
    view.side = Side::Player;
    view.x = 44;
    view.y = 41;
    view.hp = hp;
    observation.own.push_back(view);
    if (factory) {
        sim::UnitView home{};
        home.id = rig.turret;
        home.kind = UnitKind::Factory;
        home.side = Side::Player;
        home.x = kHomeX;
        home.y = kHomeY;
        home.hp = sim::kFactoryHp;
        observation.own.push_back(home);
    }
    return observation;
}

/// Select `unit` and right-click `target` with the mark on it: the unit is ordered to attack it.
void order_attack(Rig& rig, Interaction& player, sim::UnitId unit, sim::UnitId target) {
    if (!player.selection().contains(unit)) { // Space toggles: a second press would deselect it
        choose(rig, player, unit);
    }
    player.handle(with_click(at(rig, target, MouseButton::Right)), rig.sim, rig.options);
    ASSERT_EQ(player.take_orders().size(), 1U);
}

TEST(Retreat, AWoundedTankIsSentToItsFactory) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(seen(rig, rig.tank_b, 20));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.tank_b);
    const auto* move = std::get_if<sim::orders::MoveTo>(&orders.at(0).payload);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->x, sim::Fixed::from_int(kHomeX));
    EXPECT_EQ(move->y, sim::Fixed::from_int(kHomeY));
}

TEST(Retreat, WithNoLivingFactoryTheTankKeepsFighting) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(seen(rig, rig.tank_b, 5, false));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Retreat, AHealedTankResumesItsRememberedOrder) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    order_attack(rig, player, rig.tank_b, rig.enemy);
    player.tend(seen(rig, rig.tank_b, 20));
    static_cast<void>(player.take_orders());
    player.tend(seen(rig, rig.tank_b, 80));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.tank_b);
    const auto* attack = std::get_if<sim::orders::Attack>(&orders.at(0).payload);
    ASSERT_NE(attack, nullptr);
    EXPECT_EQ(attack->target, rig.enemy);
}

TEST(Retreat, ATankStillWoundedBetweenTheThresholdsStaysHome) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    order_attack(rig, player, rig.tank_b, rig.enemy);
    player.tend(seen(rig, rig.tank_b, 20));
    static_cast<void>(player.take_orders());
    player.tend(seen(rig, rig.tank_b, 79));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Retreat, AHealedTankWithNoRememberedOrderStaysIdleAndCommandable) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    choose(rig, player, rig.tank_b);
    player.tend(seen(rig, rig.tank_b, 10));
    static_cast<void>(player.take_orders());
    player.tend(seen(rig, rig.tank_b, 90));
    EXPECT_TRUE(player.take_orders().empty());
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U) << "still orderable";
}

TEST(Retreat, TheDrivenTankNeverRetreats) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(seen(rig, rig.tank_a, 1));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Retreat, ATankAboveTheThresholdIsLeftAlone) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    order_attack(rig, player, rig.tank_b, rig.enemy);
    player.tend(seen(rig, rig.tank_b, 21));
    EXPECT_TRUE(player.take_orders().empty());
    player.tend(seen(rig, rig.tank_b, 100));
    EXPECT_TRUE(player.take_orders().empty()) << "and a tank that never retreated never resumes";
}

TEST(Retreat, AnOrderGivenWhileRetreatingIsTheOneResumed) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    order_attack(rig, player, rig.tank_b, rig.enemy);
    player.tend(seen(rig, rig.tank_b, 15));
    static_cast<void>(player.take_orders());
    order_attack(rig, player, rig.tank_b, rig.enemy2);
    player.tend(seen(rig, rig.tank_b, 85));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    const auto* attack = std::get_if<sim::orders::Attack>(&orders.at(0).payload);
    ASSERT_NE(attack, nullptr);
    EXPECT_EQ(attack->target, rig.enemy2);
}

TEST(Retreat, StopForgetsTheRememberedOrder) {
    // The only Stop the player's hands make is the swap's: the tank he leaves is told to Stop. Give
    // tank_b an order, drive it, then leave it again -- its old order must not come back.
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    order_attack(rig, player, rig.tank_b, rig.enemy);
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    ASSERT_EQ(player.driven(), rig.tank_b);
    static_cast<void>(player.take_orders());
    static_cast<void>(at(rig, rig.tank_a));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    ASSERT_EQ(player.driven(), rig.tank_a);
    static_cast<void>(player.take_orders());
    player.tend(seen(rig, rig.tank_b, 20));
    static_cast<void>(player.take_orders());
    player.tend(seen(rig, rig.tank_b, 80));
    EXPECT_TRUE(player.take_orders().empty());
}

} // namespace protekt::game
