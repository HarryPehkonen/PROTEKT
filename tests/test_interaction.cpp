#include "protekt/game/interaction.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>
#include <variant>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/sounds.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

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
    RowState row;         ///< the command row over the rig's sim, as the app would hold it
};

/// A known layout, so a click test knows which pixel belongs to which unit.
///
/// NONE of these may share the camera's x: the camera sits at tile (40,30) looking down +Z, so a
/// unit at x=40 is directly behind whatever is in front of it on that line, and a click on it lands
/// on the near unit instead. The first version of this rig put the turret and the enemy at x=40
/// behind the driven tank, and five tests failed for exactly that reason -- the picker was right.
[[nodiscard]] Rig make_rig(UnitKind second = UnitKind::Tank) {
    Rig rig{};
    rig.sim = sim::Sim::create(Scenario{kSeed,
                                        {{UnitKind::Tank, Side::Player, 40, 40},
                                         {second, Side::Player, 44, 41},
                                         {UnitKind::Turret, Side::Player, 36, 41},
                                         {UnitKind::Tank, Side::Enemy, 38, 44},
                                         {UnitKind::Tank, Side::Enemy, 46, 44}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.kind == UnitKind::Tank && unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.kind == second && unit.side == Side::Player) {
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
    rig.row.update(rig.sim, Side::Player, 0);
    rig.options.row = &rig.row;
    return rig;
}

/// Both of the rig's tanks under F1, as if they had been built as one shift batch. Every unit the
/// sim can produce today is its own group, so a multi-unit group is arranged here by hand: this is
/// what the row will hold once batches exist, and the selection and the orders do not care how it
/// came to be.
void batch_tanks(Rig& rig) {
    rig.row.slots.at(group_slot(1)).units = {rig.tank_a, rig.tank_b};
    rig.row.slots.at(group_slot(2)).units.clear();
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

TEST(Interaction, ShiftSpaceDoesNotAddToTheSelection) {
    // U3: there is no Shift+Space "add one" gesture. Space is the selector and Shift+Space is the
    // same selector: a selection comes from a function key (a whole group) or from Space over one
    // vehicle starting from empty. The shift modifier changes nothing.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
    choose(rig, player, rig.turret, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "Shift+Space over a turret does not add it to the selection";
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
    rig.options.radius_tiles = 100; // the enemy stands 50 tiles out: draw it, so it can be picked
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
    // The shot timer limits the TRIGGER and nothing else: an order given right after a shot still
    // goes through. (What the right button does right after a shot is in the RightClick suite: it
    // switches the weapon, fires nothing.)
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    static_cast<void>(player.take_shot_voice());
    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U) << "the move order went through anyway";
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
    // F1 selects the tank group: tank_a (driven) and tank_b.
    batch_tanks(rig);
    player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);
    player.handle(with_key(input::kKeyTab), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
}

// ---------------------------------------------------------------- space and T

TEST(Interaction, SpaceSelectsAFriendlyUnitWhenNothingIsSelected) {
    // U3: Space is the SELECTOR. With nothing selected, over your own tank within 80% of the
    // radar's range (32 tiles), it selects that single unit. It never ADDS to a selection: a
    // group comes from a function key, or from Space over one vehicle starting from empty.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));

    // Over a turret with a selection up: turret is neither a tank (follow) nor hostile (attack),
    // so Space does nothing and the selection is untouched.
    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "Space does not add a turret to the selection";
    EXPECT_TRUE(player.message().empty());

    // Over the already-selected tank: Space follows it (it is a tank), and the selection stays.
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "Space on an already-selected tank follows it, does not toggle it out";
    EXPECT_FALSE(player.message().empty());
}

TEST(Interaction, PlainSpaceSelectsTheUnitUnderTheCrosshair) {
    // U3: with nothing selected, Space selects the single unit under the crosshair.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
}

TEST(Interaction, PlainSpaceOnAnAlreadySelectedTankKeepsItSelected) {
    // U3: Space does not toggle a unit out of the selection. On an already-selected tank it
    // follows it; the selection stays exactly as it was.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "Space on a selected tank follows it, does not remove it";
}

TEST(Interaction, TwoSpacesOnTheSameTankAreNetNothing) {
    // U3: Space on an already-selected tank follows it; the selection is unchanged.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), before);
}

TEST(Interaction, SpaceOnTheDrivenTankFollowsIt) {
    // U3: with a selection up, Space over the driven tank follows it (it is a tank), and the
    // selection is unchanged. Following the driven tank is the point: after an X-swap the wingman
    // still shadows the old hull.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);
    choose(rig, player, rig.tank_a);
    EXPECT_EQ(selected(player), before) << "following does not change the selection";
    EXPECT_FALSE(player.message().empty()) << "it says it is following";
}

TEST(Interaction, SpaceOverGroundOrNothingLeavesTheSelectionAlone) {
    // U3: ground and no-aim are ignored (not refused); a hostile with a selection is attacked.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);
    ASSERT_EQ(before.size(), 1U);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "ground";
    EXPECT_TRUE(player.message().empty()) << "ignored, not refused";
    EXPECT_TRUE(player.take_orders().empty());

    rig.options.aim.reset();
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "no aim point";
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(HoverTank, SpaceSelectsItAndTheMarkKeysOrderItLikeATank) {
    // The orders belong to the KEYS since 2026-10-23 -- `M` to move, SPACE to attack -- because the
    // right button changes weapons and nothing else now. What a hover tank still has to prove is
    // that the keys treat it like any other tank.
    Rig rig = make_rig(UnitKind::HoverTank);
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    const std::vector<Order> moved = player.take_orders();
    ASSERT_EQ(moved.size(), 1U);
    EXPECT_EQ(moved.at(0).unit, rig.tank_b);
    EXPECT_EQ(moved.at(0).kind(), OrderKind::MoveTo);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    const std::vector<Order> attack = player.take_orders();
    ASSERT_EQ(attack.size(), 1U);
    EXPECT_EQ(attack.at(0).unit, rig.tank_b);
    EXPECT_EQ(attack.at(0).kind(), OrderKind::Attack);
}

TEST(Interaction, SpaceOrdersAnAttackFromEverySelectedUnitAndKeepsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    select_and_drive(rig, player, rig.tank_b);
    // F1 selects the tank group: tank_a (now ordinary) and tank_b (driven).
    batch_tanks(rig);
    player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "the driven tank is not ordered";
    EXPECT_EQ(orders.front().unit, rig.tank_a);
    EXPECT_EQ(orders.front().kind(), OrderKind::Attack);
    EXPECT_EQ(std::get<sim::orders::Attack>(orders.front().payload).target, rig.enemy);
}

TEST(Interaction, TWithNothingSelectedIsRefusedAndSaysWhy) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "Nothing is selected to give an order to.");
}

TEST(Interaction, TOffAHostileArmsTheListInsteadOfRefusing) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "ground";
    // BZ2A-1: no longer a refusal -- with hostiles about, T offers them by number instead.
    EXPECT_TRUE(player.attack_armed());

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "own unit";
    EXPECT_TRUE(player.attack_armed());
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

TEST(Interaction, SpaceRetargetsWithoutReselecting) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    const std::vector<Order> first = player.take_orders();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(std::get<sim::orders::Attack>(first.front().payload).target, rig.enemy);

    static_cast<void>(at(rig, rig.enemy2));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
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

// ---------------------------------------------------------------- the right button (the switch)

TEST(RightClick, CyclesTheWeaponAndGivesNoOrderEvenWithASelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    ASSERT_EQ(player.weapon(), Weapon::Gun);

    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Rocket) << "the right button is the weapon switch now";
    EXPECT_TRUE(player.take_orders().empty()) << "and it orders nothing";
    EXPECT_TRUE(player.message().empty()) << "there is nothing left to refuse";

    player.handle(with_click(at(rig, rig.enemy, MouseButton::Right)), rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Mortar) << "the same click, over a hostile";
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "a weapon change is not a selection change";
}

TEST(RightClick, SwitchesTheWeaponWithNoMarkAndNothingSelected) {
    // A weapon change is not a shot: it needs no crosshair on anything, and no selection either.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    rig.options.aim.reset();
    player.handle(with_click(Click{640, 700, MouseButton::Right, true, false}), rig.sim,
                  rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Rocket);
    EXPECT_TRUE(player.selection().empty());
}

TEST(RightClick, WrapsBackToTheGunAndLeavesTheSelectionAlone) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    // Every click of the wrap, not just the last one: "the clicks land back on the gun" is also
    // true of a button that does nothing at all (found by sabotaging the switch).
    const std::array<Weapon, 5> want{Weapon::Rocket, Weapon::Mortar, Weapon::Bouncer,
                                     Weapon::Shotgun, Weapon::Gun};
    for (std::size_t click = 0; click < want.size(); ++click) {
        player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
        EXPECT_EQ(player.weapon(), want.at(click)) << "click " << click;
    }
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
}

TEST(MoveKey, MOrdersEverySelectedUnitToTheGroundUnderTheMark) {
    // U3: a selection comes from a function key (a whole group) or from Space over one vehicle.
    // F1 selects the tank group: tank_a (driven) and tank_b. The order must go to tank_b only.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    batch_tanks(rig);
    player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);
    rig.options.aim = Aim{640.0F, 700.0F};
    const Pick ground = pick(rig.sim, rig.options, 640.0F, 700.0F);
    ASSERT_TRUE(ground.has_tile());

    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "the driven tank is not ordered";
    ASSERT_EQ(orders.front().kind(), OrderKind::MoveTo);
    const auto& move = std::get<sim::orders::MoveTo>(orders.front().payload);
    EXPECT_EQ(move.x, sim::Fixed::from_int(ground.tile_x));
    EXPECT_EQ(move.y, sim::Fixed::from_int(ground.tile_y));
    EXPECT_EQ(orders.front().unit, rig.tank_b);
}

TEST(MoveKey, SpaceSelectsAPlayerTurretAndMMovesIt) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.turret);
    ASSERT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}));
    rig.options.aim = Aim{640.0F, 700.0F};
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, rig.turret);
    EXPECT_EQ(orders.front().kind(), OrderKind::MoveTo);
    const std::int64_t before = rig.sim.find(rig.turret)->x.raw + rig.sim.find(rig.turret)->y.raw;
    rig.sim.enqueue(orders.front());
    for (int t = 0; t < sim::kUnmountTicks + 5; ++t) { // it unmounts first
        rig.sim.step();
    }
    EXPECT_NE(rig.sim.find(rig.turret)->x.raw + rig.sim.find(rig.turret)->y.raw, before)
        << "the turret crawls to the ground under the mark";
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

TEST(FollowKey, FOrdersEverySelectedUnitToFollowTheUnitUnderTheMark) {
    // U3: F1 selects the tank group (tank_a driven + tank_b); the order goes to tank_b only.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    batch_tanks(rig);
    player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);
    static_cast<void>(at(rig, rig.tank_a)); // the mark on the driven tank: the wingman case
    const Aim aim = mark_of(rig);
    const Pick picked = pick(rig.sim, rig.options, aim.x, aim.y);
    ASSERT_TRUE(picked.has_unit());

    player.handle(with_key(input::kKeyF), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "the driven tank is not ordered";
    EXPECT_EQ(orders.front().kind(), OrderKind::Follow);
    EXPECT_EQ(std::get<sim::orders::Follow>(orders.front().payload).target, rig.tank_a);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
}

TEST(FollowKey, FOnAHostileIssuesFollowOrdersToo) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyF), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), OrderKind::Follow);
    EXPECT_EQ(std::get<sim::orders::Follow>(orders.front().payload).target, rig.enemy);
}

TEST(FollowKey, FOnTheGroundIssuesNoOrders) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    rig.options.aim = Aim{640.0F, 700.0F}; // open ground, as the MoveKey tests use
    ASSERT_TRUE(pick(rig.sim, rig.options, 640.0F, 700.0F).has_tile());
    ASSERT_FALSE(pick(rig.sim, rig.options, 640.0F, 700.0F).has_unit());
    player.handle(with_key(input::kKeyF), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(FollowKey, FWithNothingSelectedIssuesNoOrders) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyF), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(FollowKey, FWithNoMarkIssuesNoOrders) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    rig.options.aim.reset();
    player.handle(with_key(input::kKeyF), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Interaction, TheDrivenTankIsNeverOrderedByAKey) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    select_and_drive(rig, player, rig.tank_b);
    // F1 selects the tank group: tank_a (ordinary) and tank_b (driven).
    batch_tanks(rig);
    player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "only the unit the player is not sitting in may be ordered";
    EXPECT_EQ(orders.front().unit, rig.tank_a);
}

// The three right-click ORDER tests that used to live here -- move on the ground, attack a hostile,
// and the two refusals -- went with the order itself (2026-10-23). What the button does instead is
// the RightClick suite above; the orders it used to give are covered by MoveKey, AttackKey and
// GoKey, which is where a player reaches them now.

TEST(Interaction, OrdersAreDrainedOnce) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
    EXPECT_TRUE(player.take_orders().empty()) << "one press must not be applied twice";
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
        // F1 selects the tank group; the driven tank is excluded from the order.
        batch_tanks(rig);
        player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
        static_cast<void>(ground_click(rig));
        player.handle(with_key(input::kKeyM), rig.sim, rig.options);
        return player.take_orders();
    };
    const std::vector<Order> first = play();
    const std::vector<Order> second = play();
    ASSERT_EQ(first.size(), second.size());
    ASSERT_EQ(first.size(), 1U) << "the tank group's orderable unit gets the order";
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

TEST(Swap, XOnAnOwnHoverTankDrivesItLikeATank) {
    // E9a: the hover tank has no drive model of its own yet, so it is driven as a tank is.
    Rig rig = make_rig(UnitKind::HoverTank);
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_b);
}

TEST(Swap, AnEnemyTankIsRefusedWithAReason) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_a);
    EXPECT_EQ(player.message(), "That unit is not yours -- you can only swap into your own.");
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Swap, XOnAnOwnTurretDrivesIt) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.turret);
    EXPECT_FALSE(player.message().empty());
}

TEST(Swap, XOnAnOwnMortarOrRocketDrivesIt) {
    for (const UnitKind kind : {UnitKind::Mortar, UnitKind::Rocket}) {
        Rig rig = make_rig(kind);
        Interaction player(rig.tank_a);
        static_cast<void>(at(rig, rig.tank_b));
        player.handle(with_key(input::kKeyX), rig.sim, rig.options);
        EXPECT_EQ(player.driven(), rig.tank_b);
    }
}

TEST(Swap, TheFactoryCannotBeDriven) {
    EXPECT_TRUE(sim::is_drivable(UnitKind::Tank));
    EXPECT_TRUE(sim::is_drivable(UnitKind::HoverTank));
    EXPECT_TRUE(sim::is_drivable(UnitKind::Mortar));
    EXPECT_TRUE(sim::is_drivable(UnitKind::Rocket));
    EXPECT_TRUE(sim::is_drivable(UnitKind::Turret));
    EXPECT_FALSE(sim::is_drivable(UnitKind::Factory));
}

TEST(Swap, XOnAnOwnScavengerDrivesItEvenThoughItIsUnarmed) {
    // Q12: `is_drivable` is about whether the thing MOVES, not whether it has a gun -- the
    // scavenger and an undeployed recycler are unarmed and drivable, the factory is not.
    Rig rig = make_rig(UnitKind::Scavenger);
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_b) << "the hauler is a vehicle like any other";
}

TEST(Swap, TheRefusalForAnImmobileBuildingNamesTheDrivingRuleAndNotArmament) {
    // Q12: the old string said "you can only swap into your own armed units", which the code does
    // not enforce -- an unarmed scavenger is drivable. A refusal has to describe the rule that
    // actually refused you.
    Rig rig = make_rig(UnitKind::Factory);
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeyX), rig.sim, rig.options);
    EXPECT_EQ(player.driven(), rig.tank_a);
    EXPECT_EQ(player.message(),
              "That cannot be driven -- you can only swap into your own vehicles.");
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
    EXPECT_EQ(player.message(), "You are already driving that unit.");
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
    // U3: after the swap tank_b is driven. F1 selects the tank group (tank_a ordinary, tank_b
    // driven); the move order goes to tank_a only.
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    select_and_drive(rig, player, rig.tank_b);
    batch_tanks(rig);
    player.handle(with_key(input::kKeyF1), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);
    static_cast<void>(player.take_orders());

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
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

/// Select `unit` and press SPACE with the mark on `target`: the unit is ordered to attack it.
void order_attack(Rig& rig, Interaction& player, sim::UnitId unit, sim::UnitId target) {
    if (!player.selection().contains(unit)) { // Space toggles: a second press would deselect it
        choose(rig, player, unit);
    }
    static_cast<void>(at(rig, target));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
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

TEST(Retreat, AWoundedHoverTankIsSentToItsFactoryLikeATank) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    sim::Observation observation = seen(rig, rig.tank_b, 20);
    observation.own.at(0).kind = UnitKind::HoverTank;
    player.tend(observation);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.tank_b);
    EXPECT_EQ(orders.at(0).kind(), OrderKind::MoveTo);
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
    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
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

// ---------------------------------------------------------------- defend in place

/// A tank_b at (44,41) that sees one hostile tank per `hostiles` entry at the given tile, on the
/// defend cadence unless `tick` says otherwise. Ids are the caller's.
struct Hostile {
    sim::UnitId id;
    std::int32_t x;
    std::int32_t y;
};

[[nodiscard]] sim::Observation guarding(const Rig& rig, const std::vector<Hostile>& hostiles,
                                        std::uint16_t hp = 100, sim::Tick tick = kDefendCadence,
                                        sim::UnitId tank = sim::UnitId{}) {
    sim::Observation observation = seen(rig, tank.valid() ? tank : rig.tank_b, hp);
    observation.tick = tick;
    for (const Hostile& hostile : hostiles) {
        sim::UnitView view{};
        view.id = hostile.id;
        view.kind = UnitKind::Tank;
        view.side = Side::Enemy;
        view.x = hostile.x;
        view.y = hostile.y;
        view.raw_x = hostile.x << 16;
        view.raw_y = hostile.y << 16;
        view.hp = 100;
        observation.enemy.push_back(view);
    }
    for (sim::UnitView& own : observation.own) {
        own.raw_x = own.x << 16;
        own.raw_y = own.y << 16;
    }
    return observation;
}

[[nodiscard]] sim::UnitId focus_of(const Order& order) {
    const auto* focus = std::get_if<sim::orders::FocusFire>(&order.payload);
    return focus != nullptr ? focus->target : sim::UnitId{};
}

TEST(Defend, AnIdleTankFiresInPlaceAtAHostileInRange) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.tank_b);
    EXPECT_EQ(focus_of(orders.at(0)), rig.enemy) << "FocusFire: no stance, no destination";
}

TEST(Defend, AnIdleHoverTankFiresInPlaceLikeATank) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    sim::Observation observation = guarding(rig, {{rig.enemy, 50, 41}});
    observation.own.at(0).kind = UnitKind::HoverTank;
    player.tend(observation);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.tank_b);
    EXPECT_EQ(focus_of(orders.at(0)), rig.enemy);
}

TEST(Defend, AHostileOutOfRangeIsLeftAlone) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy, 44 + 13, 41}}));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Defend, ItThinksOnTheFifteenTickBeatAndNoOther) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}, 100, kDefendCadence + 1));
    EXPECT_TRUE(player.take_orders().empty());
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}, 100, 2 * kDefendCadence));
    EXPECT_EQ(player.take_orders().size(), 1U);
}

TEST(Defend, AStandingMoveOrderKeepsMovingAndNothingIsEmitted) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    choose(rig, player, rig.tank_b);
    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyM), rig.sim, rig.options);
    ASSERT_EQ(player.take_orders().size(), 1U);
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Defend, TheDrivenTankNeverDefendsThisWay) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy, 44, 44}}, 100, kDefendCadence, rig.tank_a));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Defend, ARetreatingTankDoesNotDefend) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}, 20));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "the retreat, and only the retreat";
    EXPECT_NE(std::get_if<sim::orders::MoveTo>(&orders.at(0).payload), nullptr);
    // Healing a little is still retreating: it stays home until return_hp.
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}, 50, 2 * kDefendCadence));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Defend, ATankBackAtReturnHpWithNoOrderDefendsAgain) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {}, 20));
    static_cast<void>(player.take_orders());
    player.tend(guarding(rig, {{rig.enemy, 50, 41}}, 80, 2 * kDefendCadence));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(focus_of(orders.at(0)), rig.enemy);
}

TEST(Defend, TheNearerOfTwoHostilesIsChosen) {
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy, 52, 41}, {rig.enemy2, 48, 41}}));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(focus_of(orders.at(0)), rig.enemy2);
}

TEST(Defend, EqualRangeGoesToTheLowerIdWhateverTheListOrder) {
    Rig rig = make_rig();
    ASSERT_LT(rig.enemy, rig.enemy2);
    Interaction player(rig.tank_a);
    player.tend(guarding(rig, {{rig.enemy2, 44, 45}, {rig.enemy, 48, 41}}));
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(focus_of(orders.at(0)), rig.enemy);
}

} // namespace protekt::game

// ---------------------------------------------------------------- the weapon switch (M1)

namespace protekt::game {
namespace {

[[nodiscard]] InputFrame with_ctrl_key(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false, true});
    return frame;
}

} // namespace

TEST(WeaponClick, TheRightButtonCyclesGunRocketMortarBouncerShotgunAndWrapsToTheGun) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    EXPECT_EQ(player.weapon(), Weapon::Gun) << "the craft starts with the gun up";
    const std::array<Weapon, 5> expected{Weapon::Rocket, Weapon::Mortar, Weapon::Bouncer,
                                         Weapon::Shotgun, Weapon::Gun};
    for (const Weapon want : expected) {
        player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
        EXPECT_EQ(player.weapon(), want);
        EXPECT_TRUE(player.message().empty()) << "the switch refuses nothing";
        EXPECT_TRUE(player.take_orders().empty()) << "choosing a weapon orders nothing";
    }
}

TEST(WeaponClick, TheKeyQNoLongerSwitchesAndNeitherDoesTheWheel) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    for (const Weapon start : kDrivenWeapons) {
        // Walk to `start` the way a player does, by right-clicking -- BOUNDED, so a switch that
        // stops working hangs nothing: it fails the assertion below instead.
        for (int step = 0; step < 8 && player.weapon() != start; ++step) {
            player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
        }
        ASSERT_EQ(player.weapon(), start) << "the right click still walks the whole wrap";
        player.handle(with_key(input::kKeyQ), rig.sim, rig.options);
        EXPECT_EQ(player.weapon(), start) << "Q is not a weapon control any more";
        InputFrame roll{};
        roll.wheel = -2;
        player.handle(roll, rig.sim, rig.options);
        EXPECT_EQ(player.weapon(), start) << "and the wheel is not either";
        player.handle(roll, rig.sim, rig.options);
        EXPECT_EQ(player.weapon(), start);
    }
}

TEST(WeaponClick, CtrlQIsStillTheQuitAndNotASwitch) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_ctrl_key(input::kKeyQ), rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Gun);
}

TEST(WeaponClick, AReleaseOrAnotherKeyDoesNotSwitch) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    InputFrame release{};
    release.keys.push_back(KeyEvent{input::kKeyQ, false, false});
    player.handle(release, rig.sim, rig.options);
    player.handle(with_key(input::kKeyTab), rig.sim, rig.options);
    EXPECT_EQ(player.weapon(), Weapon::Gun);
}

TEST(WeaponClick, TheLeftButtonFiresAndLeavesTheWeaponAlone) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    static_cast<void>(player.take_orders());
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_FALSE(player.take_orders().empty()) << "the trigger still fires";
    EXPECT_EQ(player.weapon(), Weapon::Gun) << "and the weapon is where it was";
}

TEST(WeaponClick, TheChoiceSurvivesASwapBecauseTheCraftCarriesIt) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    ASSERT_EQ(player.weapon(), Weapon::Mortar);
    select_and_drive(rig, player, rig.tank_b);
    EXPECT_EQ(player.weapon(), Weapon::Mortar);
}

// ---------------------------------------------------------------- ROW-MOVE

TEST(Interaction, ShiftFKeyMovesTheSelectionAndDoesNotSelect) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_key(input::kKeyF1 + 1), rig.sim, rig.options); // F2: tank_b
    ASSERT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
    ASSERT_EQ(rig.row.slots.at(group_slot(1)).units, (std::vector<sim::UnitId>{rig.tank_a}));

    player.handle(with_key(input::kKeyF1, true), rig.sim, rig.options); // shift + F1

    EXPECT_EQ(rig.row.slots.at(group_slot(1)).units,
              (std::vector<sim::UnitId>{rig.tank_a, rig.tank_b}))
        << "the selected tank moved into F1, after the one F1 held";
    EXPECT_FALSE(rig.row.occupied(group_slot(2))) << "and F2 went blank";
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "the press moved the selection's units and did not select the group";
}

TEST(Interaction, BareFKeyStillSelectsTheGroup) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_key(input::kKeyF1 + 1), rig.sim, rig.options); // F2: tank_b
    const RowState before = rig.row;

    player.handle(with_key(input::kKeyF1), rig.sim, rig.options); // bare F1: tank_a

    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_a}));
    EXPECT_EQ(rig.row.slots, before.slots) << "a bare press never moves anything";
}

TEST(Interaction, RefusalReachesTheNoticeLine) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_key(input::kKeyF1 + 2), rig.sim, rig.options); // F3: the turret
    ASSERT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}));
    const RowState before = rig.row;

    player.handle(with_key(input::kKeyF1, true), rig.sim, rig.options); // shift + F1: the tanks

    EXPECT_EQ(player.message(), move_refusal_message(RowMove::DifferentKind));
    EXPECT_NE(player.message(), empty_key_message(1)) << "F1 is not empty; the kinds differ";
    EXPECT_EQ(rig.row.slots, before.slots);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}));
}

TEST(Interaction, ShiftFKeyWithNothingSelectedOrNoRowIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    const RowState before = rig.row;

    player.handle(with_key(input::kKeyF1 + 7, true), rig.sim, rig.options);
    EXPECT_EQ(player.message(), move_refusal_message(RowMove::NothingToMove));
    EXPECT_EQ(rig.row.slots, before.slots);

    player.handle(with_key(input::kKeyF1 + 1), rig.sim, rig.options); // F2 selects tank_b
    rig.options.row = nullptr;
    player.handle(with_key(input::kKeyF1 + 7, true), rig.sim, rig.options);
    EXPECT_EQ(player.message(), move_refusal_message(RowMove::NothingToMove));
    EXPECT_EQ(rig.row.slots, before.slots);
}

// --------------------------------------------------------------------------------- build queue

namespace {

/// A tank to drive and a factory to build from, and nothing else. The factory is brought up with
/// the top menu's own `2`, so a test goes through the same door the player does.
struct BuildRig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank;
    sim::UnitId factory;
    Interaction player;
};

[[nodiscard]] BuildRig make_build_rig(const Scenario& scenario
                                      = Scenario{kSeed,
                                                 {{UnitKind::Tank, Side::Player, 40, 40},
                                                  {UnitKind::Factory, Side::Player, 46, 41}}}) {
    sim::Sim sim = sim::Sim::create(scenario);
    const sim::UnitId tank = sim.units().at(0).id;
    const sim::UnitId factory = sim.units().at(1).id;
    BuildRig rig{std::move(sim), SceneOptions{}, tank, factory, Interaction{tank}};
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.player.handle(with_key(input::kKey1 + 1), rig.sim, rig.options); // 2: the factory
    return rig;
}

/// The same rig with three haulers and wreckage to fetch, so a queue can actually FINISH: a test
/// of what happens once a batch is done needs scrap to pay for it.
[[nodiscard]] BuildRig make_funded_build_rig() {
    Scenario scenario{kSeed,
                      {{UnitKind::Tank, Side::Player, 40, 40},
                       {UnitKind::Factory, Side::Player, 52, 45},
                       {UnitKind::Scavenger, Side::Player, 48, 47},
                       {UnitKind::Scavenger, Side::Player, 48, 48},
                       {UnitKind::Scavenger, Side::Player, 48, 49}}};
    scenario.objectives = {};
    const sim::Fixed half = sim::Fixed::from_raw(sim::Fixed::kOne / 2);
    for (int i = 0; i < 90; ++i) {
        scenario.scrap.push_back(
            sim::Scrap{sim::fixed_add(sim::Fixed::from_int(42 + (i % 10)), half),
                       sim::fixed_add(sim::Fixed::from_int(36 + (i / 10)), half)});
    }
    return make_build_rig(scenario); // the parameter is a const reference: a move would do nothing
}

/// Steps the sim, with no input, until `rig`'s factory has nothing queued: its batch is done.
[[nodiscard]] bool run_batch_out(BuildRig& rig) {
    for (int tick = 0; tick < 20000; ++tick) {
        rig.sim.step();
        if (rig.sim.find(rig.factory)->queue.count == 0) {
            return true;
        }
    }
    return false;
}

/// The digit key for build row `row` (1-7), shifted or not.
[[nodiscard]] InputFrame build_key(int row, bool shift) {
    return with_key(input::kKey1 + row - 1, shift);
}

[[nodiscard]] const sim::orders::Build* build_of(const Order& order) {
    return std::get_if<sim::orders::Build>(&order.payload);
}

/// One frame of the app, as the loop runs it: the key is handled, the orders go to the sim, the sim
/// steps. Returns the orders that frame made.
[[nodiscard]] std::vector<Order> press(BuildRig& rig, int row, bool shift) {
    rig.player.handle(build_key(row, shift), rig.sim, rig.options);
    std::vector<Order> orders = rig.player.take_orders();
    for (const Order& order : orders) {
        rig.sim.enqueue(order);
    }
    rig.sim.step();
    return orders;
}

} // namespace

TEST(Interaction, APlainBuildKeyOrdersOneLoneBuildEvenWhenTheBankIsEmpty) {
    // The row is dimmed for want of scrap, and that must NOT stop the order: a queue that runs
    // short of scrap pauses, it is not refused. Row 1 is the Harvester, a scavenger.
    BuildRig rig = make_build_rig();
    ASSERT_EQ(rig.sim.ore(Side::Player), 0);
    ASSERT_EQ(selected(rig.player), (std::vector<sim::UnitId>{rig.factory}));
    const std::vector<Order> orders = press(rig, 1, false);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, rig.factory) << "the producer is the selected building";
    const auto* build = build_of(orders.front());
    ASSERT_NE(build, nullptr);
    EXPECT_EQ(build->kind, UnitKind::Scavenger);
    EXPECT_EQ(build->batch, 0U) << "a lone build asks the sim for a batch of its own";
    EXPECT_TRUE(rig.player.message().empty());
    EXPECT_EQ(rig.sim.find(rig.factory)->queue.count, 1U) << "and it is in the factory's queue";
}

TEST(Interaction, ShiftBuildKeysOfOneGestureShareOneTokenAndABarePressDoesNotJoinIt) {
    BuildRig rig = make_build_rig();
    std::vector<Order> orders;
    for (int n = 0; n < 3; ++n) {
        const std::vector<Order> made = press(rig, 1, true);
        orders.insert(orders.end(), made.begin(), made.end());
    }
    ASSERT_EQ(orders.size(), 3U);
    const auto* first = build_of(orders.at(0));
    ASSERT_NE(first, nullptr);
    EXPECT_NE(first->batch, 0U) << "a shift-queued run carries a shared, non-zero token";
    EXPECT_LE(first->batch, sim::kMaxBatchToken) << "an order can only carry so large a token";
    EXPECT_EQ(build_of(orders.at(1))->batch, first->batch);
    EXPECT_EQ(build_of(orders.at(2))->batch, first->batch);
    EXPECT_TRUE(press(rig, 1, false).empty())
        << "a bare press would start a lone build beside the running batch: refused";
    EXPECT_EQ(rig.sim.find(rig.factory)->queue.count, 3U);
}

TEST(Interaction, TwoGesturesOfOneProducerGetTwoTokensOnceTheFirstBatchIsDone) {
    BuildRig rig = make_funded_build_rig();
    const auto token_of_gesture = [&rig](int row) {
        const std::vector<Order> made = press(rig, row, true);
        EXPECT_EQ(made.size(), 1U);
        return made.empty() ? 0U : build_of(made.front())->batch;
    };
    const std::uint32_t first = token_of_gesture(1);
    ASSERT_TRUE(run_batch_out(rig)) << "the batch never finished";
    rig.player.handle(with_key(input::kKey1 + 8), rig.sim,
                      rig.options); // 9: no row, ends a gesture
    (void)rig.player.take_orders();
    const std::uint32_t second = token_of_gesture(2);
    EXPECT_NE(first, 0U);
    EXPECT_NE(second, 0U) << "with the first batch done, a new one is taken";
    EXPECT_NE(second, first) << "two gestures must not share one function key";
}

TEST(Interaction, ABarePressAfterTheBatchIsDoneBuildsALoneUnit) {
    BuildRig rig = make_funded_build_rig();
    ASSERT_EQ(press(rig, 1, true).size(), 1U);
    ASSERT_TRUE(run_batch_out(rig));
    const std::vector<Order> orders = press(rig, 2, false);
    ASSERT_EQ(orders.size(), 1U) << "an idle producer takes any single build";
    EXPECT_EQ(build_of(orders.front())->batch, 0U);
    EXPECT_TRUE(rig.player.message().empty());
}

TEST(Interaction, ShiftingAnotherKindIntoARunIsRefusedAloudAndTheRunGoesOn) {
    // THE RULING: SHIFT+scavenger then SHIFT+scout in one run must not put both in one batch.
    BuildRig rig = make_build_rig();
    ASSERT_EQ(press(rig, 1, true).size(), 1U);
    ASSERT_TRUE(rig.player.message().empty());
    EXPECT_TRUE(press(rig, 2, true).empty()) << "a batch is one kind";
    EXPECT_FALSE(rig.player.message().empty()) << "and the refusal is said, not silent";
    EXPECT_NE(rig.player.message(), "The build queue is full.")
        << "the queue has room; the reason is the kind";
    EXPECT_EQ(rig.sim.find(rig.factory)->queue.count, 1U);
    EXPECT_EQ(press(rig, 1, true).size(), 1U) << "the run is still open for its own kind";
}

TEST(Interaction, ShiftingAnotherKindInTheSameFrameIsRefusedToo) {
    // The sim has not seen the first press yet, so the gesture itself must remember its kind.
    BuildRig rig = make_build_rig();
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{input::kKey1, true, true});
    frame.keys.push_back(KeyEvent{input::kKey1 + 1, true, true});
    rig.player.handle(frame, rig.sim, rig.options);
    EXPECT_EQ(rig.player.take_orders().size(), 1U);
    EXPECT_FALSE(rig.player.message().empty());
}

TEST(Interaction, APressThatWouldStartAnotherBatchIsRefusedAloudWhileOneRuns) {
    // A running batch is not disturbed: a bare press, or a shifted press that begins a new run,
    // is refused when it is made -- so Harri hears it then and not when the order is dropped.
    BuildRig rig = make_build_rig();
    ASSERT_EQ(press(rig, 1, false).size(), 1U);
    ASSERT_TRUE(rig.player.message().empty());
    EXPECT_TRUE(press(rig, 1, false).empty()) << "a second lone press waits for the first";
    EXPECT_FALSE(rig.player.message().empty());
    EXPECT_NE(rig.player.message(), "The build queue is full.");
    EXPECT_TRUE(press(rig, 2, false).empty()) << "...whatever the kind";
    EXPECT_TRUE(press(rig, 2, true).empty()) << "...and a shifted press that begins a new run";
    EXPECT_FALSE(rig.player.message().empty());
    EXPECT_EQ(rig.sim.find(rig.factory)->queue.count, 1U);
}

TEST(Interaction, ASecondBarePressInOneFrameIsRefusedBeforeTheSimHasSeenTheFirst) {
    BuildRig rig = make_build_rig();
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{input::kKey1, true, false});
    frame.keys.push_back(KeyEvent{input::kKey1, true, false});
    rig.player.handle(frame, rig.sim, rig.options);
    EXPECT_EQ(rig.player.take_orders().size(), 1U) << "the orders made this frame count as queued";
    EXPECT_FALSE(rig.player.message().empty());
}

TEST(Interaction, ARunOpenedAfterABarePressIsRefusedNotJoinedToIt) {
    BuildRig rig = make_build_rig();
    ASSERT_EQ(press(rig, 1, false).size(), 1U);
    EXPECT_TRUE(press(rig, 1, true).empty())
        << "the lone build is a batch of one with an id of its own; a token cannot join it";
    EXPECT_FALSE(rig.player.message().empty());
}

TEST(Interaction, TheEleventhShiftPressIsRefusedAloudAndOrdersNothing) {
    BuildRig rig = make_build_rig();
    for (std::size_t n = 1; n <= sim::kBuildQueueDepth; ++n) {
        ASSERT_EQ(press(rig, 1, true).size(), 1U) << "press " << n << " is within the cap";
        ASSERT_TRUE(rig.player.message().empty());
    }
    ASSERT_EQ(rig.sim.find(rig.factory)->queue.count, sim::kBuildQueueDepth);
    EXPECT_TRUE(press(rig, 1, true).empty()) << "ten fill the queue; the eleventh is not an entry";
    EXPECT_FALSE(rig.player.message().empty()) << "a refused press says why on the notice line";
    EXPECT_EQ(rig.sim.find(rig.factory)->queue.count, sim::kBuildQueueDepth);
}

TEST(Interaction, ElevenShiftPressesInOneFrameStillMakeTenOrders) {
    // The sim has not seen the first ten yet, so the cap has to count the orders made earlier in
    // the same frame as well as the entries the queue already holds.
    BuildRig rig = make_build_rig();
    InputFrame frame{};
    for (int n = 0; n < 11; ++n) {
        frame.keys.push_back(KeyEvent{input::kKey1, true, true});
    }
    rig.player.handle(frame, rig.sim, rig.options);
    EXPECT_EQ(rig.player.take_orders().size(), sim::kBuildQueueDepth);
    EXPECT_FALSE(rig.player.message().empty());
}

TEST(Interaction, ABuildRowThatNamesNoKindOrdersNothing) {
    // Row 4 is the Hauler, which no UnitKind is yet: the key is drawn and does not act.
    BuildRig rig = make_build_rig();
    EXPECT_TRUE(press(rig, 4, false).empty());
    EXPECT_TRUE(press(rig, 4, true).empty());
    EXPECT_EQ(rig.sim.find(rig.factory)->queue.count, 0U);
}

// ---------------------------------------------------------------- VIS: what can be picked is what
// is drawn

namespace {

/// A rig for the availability rule: the VIEWER at tile (40,40) with the camera over its tile, a
/// second own tank beside it to hold a selection, one hostile and one far own tank where the test
/// puts them, and `radius_tiles` as the test sets it. Nothing else stands in any ray.
struct VisRig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId viewer{};
    sim::UnitId wingman{};
    sim::UnitId hostile{};
    sim::UnitId far_own{};
};

[[nodiscard]] VisRig make_vis_rig(UnitKind viewer_kind, int hostile_x, int hostile_y, int far_x,
                                  int far_y, int radius_tiles) {
    VisRig rig{};
    rig.sim = sim::Sim::create(Scenario{kSeed,
                                        {{viewer_kind, Side::Player, 40, 40},
                                         {UnitKind::Tank, Side::Player, 36, 41},
                                         {UnitKind::Tank, Side::Enemy, hostile_x, hostile_y},
                                         {UnitKind::Tank, Side::Player, far_x, far_y}}});
    rig.viewer = rig.sim.units().at(0).id;
    rig.wingman = rig.sim.units().at(1).id;
    rig.hostile = rig.sim.units().at(2).id;
    rig.far_own = rig.sim.units().at(3).id;
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.radius_tiles = radius_tiles;
    rig.options.viewer_unit = rig.viewer;
    rig.options.viewer_side = Side::Player;
    // Over the viewer's own tile, so "N tiles" from the camera is N tiles from the viewer.
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 80.0F, 0.0F, -0.12F};
    return rig;
}

/// The mark on `unit`, and what `pick` answers there.
[[nodiscard]] Pick aim_at(VisRig& rig, sim::UnitId unit) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
    return pick(rig.sim, rig.options, p.x, p.y);
}

[[nodiscard]] std::vector<sim::UnitId> named_by_t(VisRig& rig) {
    Interaction player{rig.viewer};
    // Select the wingman by aiming at it, then open the list.
    static_cast<void>(aim_at(rig, rig.wingman));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    std::vector<sim::UnitId> ids;
    for (const TargetRow& row : player.attack_targets()) {
        ids.push_back(row.id);
    }
    return ids;
}

} // namespace

TEST(Availability, AHostileJustBeyondTheDrawnRadiusCannotBePickedMarkedOrNamed) {
    // 25 tiles out, against a 24-tile drawn radius and a 40-tile radar: the world draws nothing
    // there, so the pick, the reticle's mark and the attack list must not know it is there.
    VisRig rig = make_vis_rig(UnitKind::Tank, 44, 65, 36, 44, 24);
    const Pick picked = aim_at(rig, rig.hostile);
    EXPECT_FALSE(picked.has_unit()) << "nothing is picked over a hostile that is not drawn";

    EXPECT_TRUE(named_by_t(rig).empty()) << "the attack list does not name what is not drawn";

    Interaction player{rig.viewer};
    static_cast<void>(aim_at(rig, rig.wingman));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options); // selects the wingman
    static_cast<void>(aim_at(rig, rig.hostile));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "Space over the undrawn hostile orders no attack";
}

TEST(Availability, TheSameHostileAtTwentyTilesIsPickedAndNamedAsBefore) {
    VisRig rig = make_vis_rig(UnitKind::Tank, 44, 60, 36, 44, 24);
    EXPECT_EQ(aim_at(rig, rig.hostile).unit, rig.hostile);
    EXPECT_EQ(named_by_t(rig), (std::vector<sim::UnitId>{rig.hostile}));

    Interaction player{rig.viewer};
    static_cast<void>(aim_at(rig, rig.wingman));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    static_cast<void>(aim_at(rig, rig.hostile));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), OrderKind::Attack);
}

TEST(Availability, TheReachBindsWhenTheDrawnRadiusIsLonger) {
    // A drawn radius of 100 draws everything here; the radar reach is then the binding limit. A
    // hostile 45 tiles out is beyond a tank's 40 and inside a scout's 80.
    {
        VisRig tank = make_vis_rig(UnitKind::Tank, 44, 85, 36, 44, 100);
        EXPECT_FALSE(aim_at(tank, tank.hostile).has_unit()) << "a tank's reach is 40";
        EXPECT_TRUE(named_by_t(tank).empty());
    }
    {
        VisRig scout = make_vis_rig(UnitKind::Scout, 44, 85, 36, 44, 100);
        EXPECT_EQ(aim_at(scout, scout.hostile).unit, scout.hostile) << "a scout's reach is 80";
        EXPECT_EQ(named_by_t(scout), (std::vector<sim::UnitId>{scout.hostile}));
    }
}

TEST(Availability, YourOwnUnitBeyondTheDrawnRadiusIsNotPickableEither) {
    // 30 tiles out of a 24-tile drawn radius: no ground under it, so nothing to pick or select.
    VisRig rig = make_vis_rig(UnitKind::Tank, 44, 60, 44, 70, 24);
    EXPECT_FALSE(aim_at(rig, rig.far_own).has_unit());
    Interaction player{rig.viewer};
    static_cast<void>(aim_at(rig, rig.far_own));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());

    // The same unit with a drawn radius that reaches it: your own side is never hidden by the
    // radar's reach, so it is pickable and selectable even past 40 tiles.
    VisRig wide = make_vis_rig(UnitKind::Tank, 44, 60, 44, 90, 100);
    EXPECT_EQ(aim_at(wide, wide.far_own).unit, wide.far_own);
    Interaction wide_player{wide.viewer};
    static_cast<void>(aim_at(wide, wide.far_own));
    wide_player.handle(with_key(input::kKeySpace), wide.sim, wide.options);
    EXPECT_EQ(selected(wide_player), (std::vector<sim::UnitId>{wide.far_own}));
}

} // namespace protekt::game
