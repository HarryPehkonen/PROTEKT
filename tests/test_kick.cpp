#include "level_ground.hpp"

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/audio/synth.hpp"
#include "protekt/game/kick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/ground.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/// A3, RECOIL: two kickback classes, the weapon's. LIGHT moves nothing in the sim -- the view
/// heaves and the muzzle flashes, and the hull's x and y do not change ("the x and y coordinates
/// don't change", Harri) -- and HEAVY drives the hull back along the line it fired on. Every test
/// here stands on LEVEL ground (`testing::level_ground`), so the only thing that can move a hull
/// that nobody drives is the kick.
namespace protekt::sim {
namespace {

constexpr int kTile = 100;

[[nodiscard]] Spawn spawn(Side side, UnitKind kind, int x, int y) {
    Spawn s{};
    s.side = side;
    s.kind = kind;
    s.tile_x = x;
    s.tile_y = y;
    return s;
}

/// One player unit of `kind`, under the player's hand, and an unarmed enemy base far away.
[[nodiscard]] Sim driven_alone(UnitKind kind, UnitId& id) {
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns.push_back(spawn(Side::Player, kind, kTile, kTile));
    scenario.spawns.push_back(spawn(Side::Enemy, UnitKind::Factory, 200, 200));
    Sim sim = Sim::create(scenario);
    id = sim.units().front().id;
    sim.enqueue(Order{id, orders::TakeControl{}});
    sim.step(); // the helm is taken; nothing has fired
    // A unit is spawned reloading: wait, by stepping, until its gun is loaded.
    for (int tick = 0; tick < 300 && sim.find(id)->cooldown > 0; ++tick) {
        sim.step();
    }
    EXPECT_EQ(sim.find(id)->cooldown, 0) << "the gun never loaded";
    return sim;
}

[[nodiscard]] orders::FireTowards east() {
    return orders::FireTowards{Fixed::from_int(1), Fixed::zero(), UnitId{}, Fixed::zero()};
}

/// Every trigger order a tank can pull, each firing east: its own gun, and the three carried
/// weapons the orders name -- the mortar's lob (pitched up), the shotgun's fan and the rocket.
[[nodiscard]] std::vector<Order> light_triggers(UnitId id) {
    const Fixed one = Fixed::from_int(1);
    return std::vector<Order>{
        Order{id, orders::FireTowards{one, Fixed::zero(), UnitId{}, Fixed::zero()}},
        Order{id, orders::FireLob{one, Fixed::zero(), UnitId{}, one}},
        Order{id, orders::FireFan{one, Fixed::zero(), UnitId{}, Fixed::zero()}},
        Order{id, orders::FireRocket{one, Fixed::zero(), UnitId{}, Fixed::zero()}},
    };
}

/// The one shot `shooter` made on the step just run; fails the test if there is not exactly one.
[[nodiscard]] Sim::Shot only_shot(const Sim& sim, UnitId shooter) {
    std::vector<Sim::Shot> mine;
    for (const Sim::Shot& shot : sim.shots()) {
        if (shot.shooter == shooter) {
            mine.push_back(shot);
        }
    }
    EXPECT_EQ(mine.size(), 1U) << "the trigger was not honoured once";
    return mine.empty() ? Sim::Shot{} : mine.front();
}

/// THE RECORDED BATTLE: the driven gun firing east on every tick it may, a second of its kind
/// beside it firing on its own at an enemy tank, on level ground, for `ticks`. When `watch` is
/// given, every shot is also turned into its cue and kicks the view, and the kicked camera is
/// computed each tick, as the game does -- which must change nothing in the sim.
struct Watched {
    game::ViewKick view;
    int cues{0};
    int heaved{0};
};
[[nodiscard]] std::uint64_t battle(UnitKind kind, int ticks, Watched* watch) {
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns.push_back(spawn(Side::Player, kind, kTile, kTile));
    scenario.spawns.push_back(spawn(Side::Player, kind, kTile, kTile + 6));
    scenario.spawns.push_back(spawn(Side::Enemy, UnitKind::Tank, kTile + 9, kTile + 6));
    scenario.spawns.push_back(spawn(Side::Enemy, UnitKind::Factory, 200, 200));
    Sim sim = Sim::create(scenario);
    const UnitId driven = sim.units().front().id;
    sim.enqueue(Order{driven, orders::TakeControl{}});
    for (int tick = 0; tick < ticks; ++tick) {
        sim.enqueue(Order{driven, east()});
        sim.step();
        if (watch == nullptr) {
            continue;
        }
        for (const Sim::Shot& shot : sim.shots()) {
            const game::FireCue cue = game::cue_of(shot);
            ++watch->cues;
            if (shot.shooter == driven) {
                watch->view.kick(cue.kick, sim.tick());
            }
        }
        const game::Camera camera
            = game::kicked_camera(game::Camera{}, watch->view.heave(sim.tick()));
        watch->heaved += camera.pitch > 0.0F ? 1 : 0;
    }
    return sim.state_hash();
}

} // namespace

// THE LIGHT CLASS -------------------------------------------------------------------------------

TEST(Kick, TheLightClassMovesNoHullButHeavesTheView) {
    // Harri's words are the spec: "the x and y coordinates don't change -- but the view moves up
    // for an instant". A change that lets a light weapon nudge the hull fails the first half.
    world::ScopedGround level(testing::level_ground());
    UnitId tank{};
    Sim sim = driven_alone(UnitKind::Tank, tank);
    ASSERT_EQ(weapon_spec(spec_of(UnitKind::Tank).weapon).kick, Kick::Light);
    const Unit before = *sim.find(tank);
    sim.enqueue(Order{tank, east()});
    sim.step();
    const Sim::Shot shot = only_shot(sim, tank);
    const Unit* after = sim.find(tank);
    EXPECT_EQ(after->x.raw, before.x.raw) << "a light weapon moved the hull in x";
    EXPECT_EQ(after->y.raw, before.y.raw) << "a light weapon moved the hull in y";
    EXPECT_EQ(after->x.raw, kTile * Fixed::kOne);
    EXPECT_EQ(after->y.raw, kTile * Fixed::kOne);

    // ... and the view DID move: up, by the light class's heave, at the tick of the shot.
    const game::FireCue cue = game::cue_of(shot);
    EXPECT_EQ(cue.kick, Kick::Light);
    game::ViewKick view;
    view.kick(cue.kick, sim.tick());
    const float heave = view.heave(sim.tick());
    EXPECT_FLOAT_EQ(heave, 0.035F) << "the light heave, two degrees";
    game::Camera camera{};
    camera.x = 200.0F;
    camera.y = 3.0F;
    camera.z = 200.0F;
    camera.yaw = 0.5F;
    camera.pitch = 0.1F;
    const game::Camera kicked = game::kicked_camera(camera, heave);
    EXPECT_FLOAT_EQ(kicked.pitch, 0.135F) << "the view moves UP";
    EXPECT_FLOAT_EQ(kicked.x, 200.0F) << "the eye itself stays where the hull put it";
    EXPECT_FLOAT_EQ(kicked.y, 3.0F);
    EXPECT_FLOAT_EQ(kicked.z, 200.0F);
    EXPECT_FLOAT_EQ(kicked.yaw, 0.5F);
    // "For an instant": falling, and gone after the class's ticks.
    EXPECT_GT(view.heave(sim.tick() + 2), 0.0F);
    EXPECT_LT(view.heave(sim.tick() + 2), heave);
    EXPECT_EQ(view.heave(sim.tick() + game::kKickLooks.at(0).ticks), 0.0F);
    EXPECT_GT(view.flash(sim.tick()), 0.0F) << "and the muzzle flashed";
}

TEST(Kick, EveryLightWeaponLeavesTheHullWhereItStood) {
    // The carried weapons a trigger order names -- the mortar's lob, the shotgun's fan, the rocket
    // -- are light too, and are fired from a tank here: none of them may move it.
    world::ScopedGround level(testing::level_ground());
    const std::size_t count = light_triggers(UnitId{}).size();
    for (std::size_t i = 0; i < count; ++i) {
        UnitId tank{};
        Sim sim = driven_alone(UnitKind::Tank, tank);
        const Unit before = *sim.find(tank);
        sim.enqueue(light_triggers(tank).at(i));
        sim.step();
        const Sim::Shot shot = only_shot(sim, tank);
        EXPECT_EQ(weapon_spec(shot.weapon).kick, Kick::Light) << "trigger " << i;
        EXPECT_NE(shot.weapon, WeaponId::None) << "trigger " << i << ": the shot names no weapon";
        EXPECT_EQ(sim.find(tank)->x.raw, before.x.raw) << "trigger " << i;
        EXPECT_EQ(sim.find(tank)->y.raw, before.y.raw) << "trigger " << i;
    }
}

TEST(Kick, TheLightClassMovesNoRecordedBattle) {
    // The battle as it was recorded BEFORE A3 existed (f0293f4: 3881818275986672575, measured on
    // that tree by the same scenario), and again while every shot is cued and every kick heaves
    // the camera: the same value both ways. If this moves, the render effect reached the sim.
    world::ScopedGround level(testing::level_ground());
    constexpr std::uint64_t kBeforeA3 = 3881818275986672575ULL;
    Watched watched;
    const std::uint64_t plain = battle(UnitKind::Tank, 120, nullptr);
    const std::uint64_t seen = battle(UnitKind::Tank, 120, &watched);
    EXPECT_EQ(plain, kBeforeA3);
    EXPECT_EQ(seen, plain) << "watching the kick changed the battle";
    EXPECT_GT(watched.cues, 8) << "the battle fires: both guns, many times";
    EXPECT_GT(watched.heaved, 0) << "and the view was heaved while it did";
}

// THE HEAVY CLASS -------------------------------------------------------------------------------

TEST(Kick, TheHeavyClassDrivesTheHullBackAQuarterTile) {
    // The walker's gun is heavy: the shot east carries the hull a quarter tile WEST, straight back
    // along the line of fire, on the tick it fires. For comparison, the shove's precedent: a tank
    // pushing another gives up a third of its step, 1/15 of a tile, on every tick it pushes.
    world::ScopedGround level(testing::level_ground());
    UnitId walker{};
    Sim sim = driven_alone(UnitKind::Walker, walker);
    ASSERT_EQ(weapon_spec(spec_of(UnitKind::Walker).weapon).kick, Kick::Heavy);
    const Unit before = *sim.find(walker);
    sim.enqueue(Order{walker, east()});
    sim.step();
    const Sim::Shot shot = only_shot(sim, walker);
    EXPECT_EQ(shot.weapon, WeaponId::WalkerGun);
    const Unit* after = sim.find(walker);
    EXPECT_EQ(after->x.raw, before.x.raw - (Fixed::kOne / 4)) << "a quarter tile back, west";
    EXPECT_EQ(after->y.raw, before.y.raw) << "and not aside";
    // The kick is spent: with nothing more fired, the hull stays where it was carried.
    const Unit kicked = *after;
    for (int tick = 0; tick < 10; ++tick) {
        sim.step();
    }
    EXPECT_EQ(sim.find(walker)->x.raw, kicked.x.raw);
    EXPECT_EQ(sim.find(walker)->y.raw, kicked.y.raw);
}

TEST(Kick, TheHeavyKickFollowsTheLineOfFireWhateverItIs) {
    // A shot along (3, 4) carries the hull back along (-3, -4), a quarter tile in all.
    world::ScopedGround level(testing::level_ground());
    UnitId walker{};
    Sim sim = driven_alone(UnitKind::Walker, walker);
    const Unit before = *sim.find(walker);
    sim.enqueue(
        Order{walker, orders::FireTowards{Fixed::from_int(3), Fixed::from_int(4), UnitId{}, {}}});
    sim.step();
    const Unit* after = sim.find(walker);
    const std::int64_t dx = after->x.raw - before.x.raw;
    const std::int64_t dy = after->y.raw - before.y.raw;
    EXPECT_NEAR(static_cast<double>(dx), -0.6 * Fixed::kOne / 4, 1.0);
    EXPECT_NEAR(static_cast<double>(dy), -0.8 * Fixed::kOne / 4, 1.0);
}

TEST(Kick, AKickIsStoppedByAVehicleBehindAndShovesNobody) {
    // The kick is not a drive: it never carries another vehicle. A tank stands a tile behind the
    // walker (spawns are a whole tile apart). The first kick carries the walker a quarter tile
    // back, to three quarters of a tile from the tank -- touching, `kBodyRadiusRaw`, not inside.
    // The second would take it inside the tank's body, so the walker takes it into its tracks and
    // stands; the tank is not touched by either.
    world::ScopedGround level(testing::level_ground());
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns.push_back(spawn(Side::Player, UnitKind::Walker, kTile, kTile));
    scenario.spawns.push_back(spawn(Side::Player, UnitKind::Tank, kTile - 1, kTile));
    scenario.spawns.push_back(spawn(Side::Enemy, UnitKind::Factory, 200, 200));
    Sim sim = Sim::create(scenario);
    const UnitId walker = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{walker, orders::TakeControl{}});
    const auto reload = [&] {
        for (int tick = 0; tick < 300 && (tick == 0 || sim.find(walker)->cooldown > 0); ++tick) {
            sim.step();
        }
        ASSERT_EQ(sim.find(walker)->cooldown, 0);
    };
    reload();
    const Unit walker_start = *sim.find(walker);
    const Unit tank_start = *sim.find(tank);
    sim.enqueue(Order{walker, east()});
    sim.step();
    (void)only_shot(sim, walker);
    EXPECT_EQ(sim.find(walker)->x.raw, walker_start.x.raw - (Fixed::kOne / 4)) << "first: back";
    EXPECT_EQ(sim.find(walker)->x.raw - sim.find(tank)->x.raw, kBodyRadiusRaw) << "touching";
    reload();
    const Unit touching = *sim.find(walker);
    sim.enqueue(Order{walker, east()});
    sim.step();
    (void)only_shot(sim, walker);
    EXPECT_EQ(sim.find(walker)->x.raw, touching.x.raw) << "second: stopped by the tank behind";
    EXPECT_EQ(sim.find(walker)->y.raw, touching.y.raw);
    EXPECT_EQ(sim.find(tank)->x.raw, tank_start.x.raw) << "and the tank was shoved by neither";
    EXPECT_EQ(sim.find(tank)->y.raw, tank_start.y.raw);
}

TEST(Kick, ABuildingsHeavyGunWouldNotMoveItEither) {
    // Every heavy weapon today rides a vehicle; this holds the rule for the case that is coming --
    // a hull that cannot move (`UnitProfile::mobile` false) is not moved by any kick, so a heavy
    // gun on a building is the building's light kick, not a sliding tower.
    for (std::size_t i = 0; i < kWeaponCount; ++i) {
        const WeaponSpec& weapon = kWeaponSpecs.at(i);
        if (weapon.kick != Kick::Heavy) {
            continue;
        }
        for (std::size_t k = 0; k < kUnitSpecs.size(); ++k) {
            if (kUnitSpecs.at(k).weapon == static_cast<WeaponId>(i)) {
                EXPECT_GT(kUnitSpecs.at(k).max_speed.raw, 0)
                    << kUnitSpecs.at(k).name << " carries a heavy gun on a hull that cannot move";
            }
        }
    }
}

// THE CLASS IS DATA -----------------------------------------------------------------------------

TEST(Kick, OneHullTwoWeaponsTwoKicksFromTheTable) {
    // The same walker fires its own gun (heavy) and then the carried rocket (light), from the same
    // spot: the first moves it, the second does not, and by EXACTLY each weapon's row -- the hull
    // is the same, so the difference is the weapon's, read from `kWeaponSpecs`, not a branch on
    // the kind.
    world::ScopedGround level(testing::level_ground());
    struct Trial {
        Order (*order)(UnitId);
        WeaponId weapon;
    };
    const std::vector<Trial> trials{
        {[](UnitId id) { return Order{id, orders::FireTowards{Fixed::from_int(1), {}, {}, {}}}; },
         WeaponId::WalkerGun},
        {[](UnitId id) { return Order{id, orders::FireRocket{Fixed::from_int(1), {}, {}, {}}}; },
         WeaponId::Rocket},
    };
    std::vector<std::int64_t> moved;
    for (const Trial& trial : trials) {
        UnitId walker{};
        Sim sim = driven_alone(UnitKind::Walker, walker);
        const Unit before = *sim.find(walker);
        sim.enqueue(trial.order(walker));
        sim.step();
        const Sim::Shot shot = only_shot(sim, walker);
        EXPECT_EQ(shot.weapon, trial.weapon);
        const std::int64_t back = before.x.raw - sim.find(walker)->x.raw;
        EXPECT_EQ(back, kick_spec(weapon_spec(trial.weapon).kick).hull_back.raw)
            << weapon_spec(trial.weapon).name << ": the hull moved by something not in its row";
        moved.push_back(back);
    }
    ASSERT_EQ(moved.size(), 2U);
    EXPECT_GT(moved.at(0), 0) << "the heavy gun moved the walker";
    EXPECT_EQ(moved.at(1), 0) << "the light rocket did not";
}

TEST(Kick, EveryArmedMovingKindIsKickedByItsWeaponsRowAndNothingElse) {
    // Every kind that drives and fires, fired once by hand: what it moved is its weapon's
    // `hull_back`, so a new heavy weapon is a row of the table and nothing more.
    world::ScopedGround level(testing::level_ground());
    for (std::size_t k = 0; k < kUnitSpecs.size(); ++k) {
        const auto kind = static_cast<UnitKind>(k);
        const UnitSpec& spec = kUnitSpecs.at(k);
        if (spec.range().raw <= 0 || spec.damage == 0 || spec.max_speed.raw <= 0
            || kind == UnitKind::Turret) {
            continue; // unarmed, a building, or a turret that must mount before it fires
        }
        UnitId id{};
        Sim sim = driven_alone(kind, id);
        const Unit before = *sim.find(id);
        sim.enqueue(Order{id, east()});
        sim.step();
        const Sim::Shot shot = only_shot(sim, id);
        EXPECT_EQ(shot.weapon, spec.weapon) << spec.name;
        EXPECT_EQ(before.x.raw - sim.find(id)->x.raw,
                  kick_spec(weapon_spec(spec.weapon).kick).hull_back.raw)
            << spec.name;
        EXPECT_EQ(sim.find(id)->y.raw, before.y.raw) << spec.name;
    }
}

// THE SOUND -------------------------------------------------------------------------------------

TEST(Kick, EveryArmedWeaponHasASoundAndUnarmedHasNone) {
    for (std::size_t i = 0; i < kWeaponCount; ++i) {
        const auto weapon = static_cast<WeaponId>(i);
        const bool armed = weapon_spec(weapon).range.raw > 0;
        EXPECT_EQ(game::fire_sound(weapon).has_value(), armed)
            << weapon_spec(weapon).name << ": a shot with no sound is a bug";
    }
}

TEST(Kick, EveryShotOfABattleCarriesItsWeaponsSound) {
    // The coupling, asserted rather than assumed: every shot the sim made in a battle becomes a
    // cue that names its weapon's class and carries its sound, read from the shot itself.
    world::ScopedGround level(testing::level_ground());
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns.push_back(spawn(Side::Player, UnitKind::Walker, kTile, kTile));
    scenario.spawns.push_back(spawn(Side::Player, UnitKind::Tank, kTile, kTile + 6));
    scenario.spawns.push_back(spawn(Side::Enemy, UnitKind::Tank, kTile + 9, kTile + 6));
    scenario.spawns.push_back(spawn(Side::Enemy, UnitKind::Factory, 200, 200));
    Sim sim = Sim::create(scenario);
    int shots = 0;
    for (int tick = 0; tick < 90; ++tick) {
        sim.step();
        for (const Sim::Shot& shot : sim.shots()) {
            ++shots;
            const Unit* shooter = sim.find(shot.shooter);
            ASSERT_NE(shooter, nullptr);
            EXPECT_EQ(shot.weapon, spec_of(shooter->kind).weapon) << "tick " << tick;
            const game::FireCue cue = game::cue_of(shot);
            EXPECT_EQ(cue.shooter, shot.shooter);
            EXPECT_EQ(cue.weapon, shot.weapon);
            EXPECT_EQ(cue.kick, weapon_spec(shot.weapon).kick);
            ASSERT_TRUE(cue.sound.has_value()) << "tick " << tick << ": a silent shot";
            EXPECT_EQ(cue.sound, std::optional{audio::SoundKind::YourShot});
        }
    }
    EXPECT_GT(shots, 4);
}

TEST(Kick, TheTriggersShotReachesTheAppWithItsSoundAndItsKick) {
    // Through the real path the game uses: the pipeline steps the sim, and hands back every shot
    // the driven unit made as a cue -- the one the app kicks the view with and the one whose sound
    // is played.
    world::ScopedGround level(testing::level_ground());
    UnitId walker{};
    Sim sim = driven_alone(UnitKind::Walker, walker);
    sim.enqueue(Order{walker, east()});
    app::AudioFrame frame{game::Listener{}, true, true, true, walker};
    game::HitFlash flash;
    game::HitMark mark;
    game::RadarActivity radar;
    const app::AudioPipeline pipeline{nullptr};
    const app::AdvanceResult result = pipeline.advance(sim, 1, frame, flash, mark, radar);
    EXPECT_FALSE(result.trigger_refused);
    ASSERT_EQ(result.fired.size(), 1U);
    EXPECT_EQ(result.fired.front().shooter, walker);
    EXPECT_EQ(result.fired.front().kick, Kick::Heavy);
    ASSERT_TRUE(result.fired.front().sound.has_value());
    EXPECT_EQ(result.fired.front().sound, std::optional{audio::SoundKind::YourShot});
}

// THE FLASH -------------------------------------------------------------------------------------

TEST(Kick, TheMuzzleFlashLightsTheWorldAndNotTheHud) {
    // A wash of the WORLD's vertices toward the muzzle's warm white; the HUD after them (here the
    // reticle) is not lit by a gun; positions never move; and 0 is the frame without it, exactly.
    // Vertices only, no rasteriser: nothing here needs a renderer.
    const Sim sim = Sim::create(Scenario{42, {{UnitKind::Tank, Side::Player, 40, 40}}});
    game::SceneOptions plain;
    plain.seed = 42;
    plain.radius_tiles = 8;
    plain.camera.x = 80.0F;
    plain.camera.z = 80.0F;
    plain.camera.y = 8.0F;
    plain.aim = game::Aim{640.0F, 360.0F};
    plain.extent = game::Extent{1280, 720};
    game::SceneOptions zero = plain;
    zero.muzzle_flash = 0.0F;
    game::SceneOptions lit = plain;
    lit.muzzle_flash = 0.5F;
    const game::Scene a = game::build_scene(sim, plain);
    const game::Scene z = game::build_scene(sim, zero);
    const game::Scene b = game::build_scene(sim, lit);
    ASSERT_EQ(a.vertices.size(), z.vertices.size());
    ASSERT_EQ(a.vertices.size(), b.vertices.size()) << "a light, not new geometry";
    ASSERT_GT(a.world_vertex_count, 0U);
    ASSERT_LT(a.world_vertex_count, a.vertices.size()) << "the rig must draw some HUD";
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        EXPECT_EQ(a.vertices.at(i).colour, z.vertices.at(i).colour) << "zero is no flash";
        EXPECT_EQ(a.vertices.at(i).position, b.vertices.at(i).position);
        for (std::size_t c = 0; c < 3; ++c) {
            const float was = a.vertices.at(i).colour.at(c);
            const float now = b.vertices.at(i).colour.at(c);
            if (i < a.world_vertex_count) {
                EXPECT_NEAR(now, was + ((game::kMuzzleFlashColour.at(c) - was) * 0.5F), 1e-5F)
                    << "world vertex " << i;
            } else {
                EXPECT_EQ(now, was) << "HUD vertex " << i << " was lit by the gun";
            }
        }
    }
}

// THE HEAVY CLASS IN A RECORDED BATTLE ----------------------------------------------------------

TEST(Kick, TheWalkerBattleIsReRecordedWithTheKick) {
    // RE-RECORDED, A3 (the heavy kick), as a stated act. The same scenario as the light battle
    // with two walkers in the tanks' places. OLD value 1553913428333780527 is the code at f0293f4,
    // before the kick, measured on that tree; the walkers' guns are heavy now, so every shot
    // carries its hull a quarter tile back and the battle is fought from where the kicks left it.
    // Restored exactly by turning the walker's gun back to LIGHT in `kWeaponSpecs` (a temporary
    // toggle, removed): 1553913428333780527 again. Over these 120 ticks the driven walker fires
    // three times and ends three quarters of a tile west of where it stood. Watching the view kick
    // does not move it again.
    world::ScopedGround level(testing::level_ground());
    Watched watched;
    const std::uint64_t plain = battle(UnitKind::Walker, 120, nullptr);
    EXPECT_NE(plain, 1553913428333780527ULL) << "the heavy kick changed nothing";
    EXPECT_EQ(plain, 17695418926854521179ULL);
    EXPECT_EQ(battle(UnitKind::Walker, 120, &watched), plain);
}

} // namespace protekt::sim
