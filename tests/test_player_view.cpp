// The app layer's eyes, radar and ears follow the tank the player is DRIVING, read fresh each
// frame. The bug these pin: the camera, radar and listener kept the id from startup, so after an
// X swap the throttle drove the new tank while the view stayed on the old one.
#include "protekt/app/player_view.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/roster.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {
namespace {

using game::Session;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    game::SceneOptions options;
    sim::UnitId tank_a{};
    sim::UnitId tank_b{};
};

Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Tank, Side::Player, 40, 40},
                                              {UnitKind::Tank, Side::Player, 60, 40},
                                              {UnitKind::Tank, Side::Enemy, 40, 70}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.side == Side::Player) {
            rig.tank_b = unit.id;
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    return rig;
}

/// A session that started driving tank A and has swapped into tank B with X.
Session swapped_session(Rig& rig) {
    Session session{rig.tank_a};
    // Aim at tank B: put the camera behind A, looking along +x at B is what the swap's pick needs.
    const sim::Unit* b = rig.sim.find(rig.tank_b);
    rig.options.camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    rig.options.camera.yaw = 1.5707964F; // +x
    const float bx = static_cast<float>(b->x.raw) / 65536.0F * game::kMetresPerTile;
    const float bz = static_cast<float>(b->y.raw) / 65536.0F * game::kMetresPerTile;
    const float ground = game::height_metres(kSeed, b->x.floor_int(), b->y.floor_int());
    const game::Projected p = game::project(rig.options.camera, kExtent, bx, ground + 0.9F, bz);
    rig.options.aim = game::Aim{p.x, p.y};
    input::InputFrame swap;
    swap.keys.push_back(input::KeyEvent{input::kKeyX, true, false});
    std::vector<sim::Order> out;
    session.frame(swap, rig.sim, rig.options, out);
    return session;
}

TEST(PlayerView, TheCameraStartsOnTheTankTheSessionDrives) {
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    EXPECT_NEAR(camera.x, 40.0F * game::kMetresPerTile, game::kMetresPerTile);
}

TEST(PlayerView, AfterASwapTheCameraIsOnTheNewTank) {
    Rig rig = make_rig();
    const Session session = swapped_session(rig);
    ASSERT_EQ(session.driven(), rig.tank_b) << "the rig must have swapped";
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    EXPECT_NEAR(camera.x, 60.0F * game::kMetresPerTile, game::kMetresPerTile)
        << "the eye stayed on the tank the player left";
}

TEST(PlayerView, AfterASwapTheRadarIsDrawnFromTheNewTank) {
    Rig rig = make_rig();
    const Session session = swapped_session(rig);
    ASSERT_EQ(session.driven(), rig.tank_b);
    EXPECT_EQ(radar_viewer(session, false), rig.tank_b);
    EXPECT_FALSE(radar_viewer(session, true).valid()) << "--no-radar means no viewer";
}

TEST(PlayerView, AfterASwapTheListenerAndTheHitFlashAreTheNewTank) {
    Rig rig = make_rig();
    const Session session = swapped_session(rig);
    ASSERT_EQ(session.driven(), rig.tank_b);
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    const AudioFrame frame = audio_frame_for(rig.sim, session, camera, false, true);
    EXPECT_EQ(frame.player, rig.tank_b);
    EXPECT_FLOAT_EQ(frame.listener.x, camera.x);
    EXPECT_FLOAT_EQ(frame.listener.z, camera.z);
    EXPECT_TRUE(frame.player_alive_now);
}

// ------------------------------------------------------------------------ the command menu

using game::MenuDim;
using game::MenuSubject;
using game::MenuTarget;

/// A base with one of everything the menu knows, tanks to drive and to fight, an enemy to look at,
/// and a wrecked-tank skirmish the hauler can bank scrap from. Unit 0 is the factory, 1 the
/// scavenger, 2 the turret, 3 and 4 the player's tanks, 5 the enemy's.
sim::Scenario base_scenario() {
    sim::Scenario s;
    s.seed = kSeed;
    s.spawns = {sim::Spawn{UnitKind::Factory, Side::Player, 52, 45},
                sim::Spawn{UnitKind::Scavenger, Side::Player, 58, 40},
                sim::Spawn{UnitKind::Turret, Side::Player, 58, 45},
                sim::Spawn{UnitKind::Tank, Side::Player, 50, 41},
                sim::Spawn{UnitKind::Tank, Side::Player, 58, 41},
                sim::Spawn{UnitKind::Tank, Side::Enemy, 57, 43}};
    return s;
}

sim::UnitId unit_id(const sim::Sim& sim, std::size_t index) { return sim.units().at(index).id; }

/// The camera on the driven tank, turned to face `id`, and the aim on it: the mark sits on the
/// unit.
void aim_at(Rig& rig, const Session& session, sim::UnitId id) {
    rig.options.camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    const sim::Unit* unit = rig.sim.find(id);
    const float x = static_cast<float>(unit->x.raw) / 65536.0F * game::kMetresPerTile;
    const float z = static_cast<float>(unit->y.raw) / 65536.0F * game::kMetresPerTile;
    rig.options.camera.yaw = std::atan2(x - rig.options.camera.x, z - rig.options.camera.z);
    const float ground = game::height_metres(kSeed, unit->x.floor_int(), unit->y.floor_int());
    const game::Projected p = game::project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = game::Aim{p.x, p.y};
}

/// A rig on `base_scenario`, driving tank 3.
Rig make_base_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(base_scenario());
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    return rig;
}

/// SPACE over `id`, which selects it: the only way a session's selection is made from one unit.
void select_with_space(Rig& rig, Session& session, sim::UnitId id) {
    aim_at(rig, session, id);
    input::InputFrame space;
    space.keys.push_back(input::KeyEvent{input::kKeySpace, true, false});
    std::vector<sim::Order> out;
    session.frame(space, rig.sim, rig.options, out);
}

TEST(PlayerView, ASelectedFactoryAsksForTheBuildList) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 0)};
    EXPECT_EQ(menu_subject_of(rig.sim, selected), MenuSubject::Deployment);
}

TEST(PlayerView, ASelectedScavengerAsksForTheHarvesterMenu) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 1)};
    EXPECT_EQ(menu_subject_of(rig.sim, selected), MenuSubject::Harvester);
}

TEST(PlayerView, ASelectedRecyclerAsksForTheRecyclerMenu) {
    sim::Scenario s = base_scenario();
    s.spawns.push_back(sim::Spawn{UnitKind::Recycler, Side::Player, 54, 41});
    Rig rig{};
    rig.sim = sim::Sim::create(s);
    const sim::UnitId recycler = unit_id(rig.sim, 6);
    ASSERT_EQ(rig.sim.find(recycler)->kind, UnitKind::Recycler);
    const std::vector<sim::UnitId> selected{recycler};
    ASSERT_EQ(menu_subject_of(rig.sim, selected), MenuSubject::Recycler);

    game::MenuInput in;
    in.subject = menu_subject_of(rig.sim, selected);
    const std::vector<game::MenuRow> rows = game::menu_rows(in);
    ASSERT_EQ(rows.size(), 6U) << "SPACE, TAB, ~, then 1 FOLLOW ME, 2 GO TO NAV, 3 DEPLOY";
    EXPECT_EQ(rows.at(3).verb, "FOLLOW ME");
    EXPECT_EQ(rows.at(4).verb, "GO TO NAV");
    EXPECT_EQ(rows.at(5).key, "3");
    EXPECT_EQ(rows.at(5).verb, "DEPLOY");
}

// RB1b: the recycler the skirmish now spawns, selected, is the recycler's menu on screen -- six
// rows, the third numbered one `3 DEPLOY` -- and the roster strip names it RECYCLER on the last
// free key.
TEST(PlayerView, TheSkirmishRecyclerIsLabelledRecyclerAndAsksForTheSixRowMenu) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    const game::Roster roster = game::roster_of(sim, Side::Player);
    ASSERT_EQ(roster.groups.size(), 7U) << "tank, hover, factory, turret, mortar, rocket, recycler";
    const game::RosterGroup& group = roster.groups.back();
    ASSERT_EQ(group.kind, UnitKind::Recycler);
    EXPECT_EQ(game::kind_label(group.kind), "RECYCLER");
    EXPECT_EQ(group.key, 7) << "F7: the last free key in id order, nothing reordered";
    ASSERT_EQ(group.units.size(), 1U);

    const std::vector<sim::UnitId> selected{group.units.front()};
    game::MenuInput in;
    in.subject = menu_subject_of(sim, selected);
    ASSERT_EQ(in.subject, game::MenuSubject::Recycler);
    const std::vector<game::MenuRow> rows = game::menu_rows(in);
    ASSERT_EQ(rows.size(), 6U) << "SPACE, TAB, ~, then 1 FOLLOW ME, 2 GO TO NAV, 3 DEPLOY";
    EXPECT_EQ(rows.at(5).key, "3");
    EXPECT_EQ(rows.at(5).verb, "DEPLOY");
}

TEST(PlayerView, ASelectedTurretAsksForTheTurretMenu) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 2)};
    EXPECT_EQ(menu_subject_of(rig.sim, selected), MenuSubject::Turret);
}

TEST(PlayerView, ATankAsksForNoMenuAtAll) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 3), unit_id(rig.sim, 4)};
    EXPECT_EQ(menu_subject_of(rig.sim, selected), MenuSubject::None)
        << "a tank's menu was never captured, and a menu must not be invented for it";
}

TEST(PlayerView, TwoKindsSelectedAtOnceAskForNoMenu) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 0), unit_id(rig.sim, 1)};
    EXPECT_EQ(menu_subject_of(rig.sim, selected), MenuSubject::None)
        << "there is one subject per menu";
}

TEST(PlayerView, NothingSelectedAsksForNoMenu) {
    const Rig rig = make_base_rig();
    EXPECT_EQ(menu_subject_of(rig.sim, std::span<const sim::UnitId>{}), MenuSubject::None);
    const std::vector<sim::UnitId> unknown{sim::UnitId{9999}};
    EXPECT_EQ(menu_subject_of(rig.sim, unknown), MenuSubject::None)
        << "an id that is not in the sim is not selected";
}

TEST(PlayerView, TheCursorOverYourOwnCraftOffersSelect) {
    Rig rig = make_base_rig();
    const Session session{unit_id(rig.sim, 3)};
    aim_at(rig, session, unit_id(rig.sim, 4));
    ASSERT_EQ(game::pick(rig.sim, rig.options, rig.options.aim->x, rig.options.aim->y).unit,
              unit_id(rig.sim, 4))
        << "the rig must be aimed at the tank";
    EXPECT_EQ(menu_target_of(rig.sim, rig.options), MenuTarget::Selectable);
}

TEST(PlayerView, TheCursorOverAnEnemyNamesNoSelect) {
    Rig rig = make_base_rig();
    const Session session{unit_id(rig.sim, 3)};
    aim_at(rig, session, unit_id(rig.sim, 5));
    ASSERT_EQ(game::pick(rig.sim, rig.options, rig.options.aim->x, rig.options.aim->y).unit,
              unit_id(rig.sim, 5))
        << "the rig must be aimed at the enemy";
    EXPECT_EQ(menu_target_of(rig.sim, rig.options), MenuTarget::Hostile);
}

TEST(PlayerView, TheCursorOverBareGroundNamesNothing) {
    Rig rig = make_base_rig();
    rig.options.camera = camera_for(rig.sim, Session{unit_id(rig.sim, 3)}, kSeed, std::nullopt);
    rig.options.camera.yaw = -1.5707964F;        // -x, away from every other unit
    rig.options.aim = game::Aim{640.0F, 650.0F}; // low on the screen, below every unit
    const game::Pick picked
        = game::pick(rig.sim, rig.options, rig.options.aim->x, rig.options.aim->y);
    ASSERT_FALSE(picked.has_unit());
    ASSERT_TRUE(picked.has_tile()) << "the rig must be aimed at ground";
    EXPECT_EQ(menu_target_of(rig.sim, rig.options), MenuTarget::Ground);
}

TEST(PlayerView, WithNoCursorAtAllTheStateRowNamesNothing) {
    Rig rig = make_base_rig();
    rig.options.aim.reset();
    EXPECT_EQ(menu_target_of(rig.sim, rig.options), MenuTarget::None);
}

TEST(PlayerView, AnArmedAttackListTakesTheSlotTheMenuWouldUse) {
    Rig rig = make_base_rig();
    Session session{unit_id(rig.sim, 3)};
    select_with_space(rig, session, unit_id(rig.sim, 0));
    ASSERT_FALSE(session.selection().empty()) << "the rig must have selected the factory";
    EXPECT_FALSE(menu_for(rig.sim, session, rig.options, false).empty());
    EXPECT_TRUE(menu_for(rig.sim, session, rig.options, true).empty());
}

TEST(PlayerView, TheMenuWeighsTheScrapTheSimHasBanked) {
    Rig rig = make_base_rig();
    Session session{unit_id(rig.sim, 3)};
    select_with_space(rig, session, unit_id(rig.sim, 0));
    ASSERT_EQ(menu_subject_of(rig.sim, session.selection().units()), MenuSubject::Deployment);
    ASSERT_EQ(rig.sim.ore(Side::Player), 0);

    // With nothing banked every build row is out of reach.
    int build_rows = 0;
    for (const game::MenuRow& row : menu_for(rig.sim, session, rig.options, false)) {
        if (row.key.size() == 1 && row.key.front() >= '1' && row.key.front() <= '7') {
            ++build_rows;
            EXPECT_FALSE(row.enabled) << row.key << " " << row.verb;
            EXPECT_EQ(row.reason, MenuDim::Unaffordable) << row.key << " " << row.verb;
        }
    }
    EXPECT_EQ(build_rows, 7);

    // The hauler banks the wreck of a fight: the ledger now reads kScrapPerWreck, and the rows
    // cheap enough for it come alive while the dear ones stay dim.
    int ticks = 0;
    while (rig.sim.ore(Side::Player) < sim::kScrapPerWreck && ticks++ < 6000) {
        rig.sim.step();
    }
    ASSERT_GE(rig.sim.ore(Side::Player), sim::kScrapPerWreck) << "the hauler never banked";
    int live = 0;
    int dim = 0;
    for (const game::MenuRow& row : menu_for(rig.sim, session, rig.options, false)) {
        if (row.key.size() == 1 && row.key.front() >= '1' && row.key.front() <= '7') {
            (row.enabled ? live : dim) += 1;
        }
    }
    EXPECT_GT(live, 0) << "scrap is banked and nothing on the list is affordable";
    EXPECT_GT(dim, 0) << "four loads do not buy everything on the list";
}

TEST(PlayerView, ADeadUnitsIdIsNotWhatTheMenuIsAbout) {
    // A corpse is not a selection. The sim does not ERASE a dead unit -- the wreckage is drawn from
    // the scrap list and the unit itself stays in the table with `alive` false -- and nothing
    // prunes a selection when a selected unit dies (only TAB and ESCAPE clear it). So a dead
    // factory's id can still sit in the player's selection, and without `menu_subject_of`'s alive
    // filter the menu would offer a build list for a wreck.
    Rig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{
        kSeed, {{UnitKind::Factory, Side::Player, 40, 40}, {UnitKind::Tank, Side::Enemy, 43, 40}}});
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    const sim::UnitId factory = unit_id(rig.sim, 0);
    int ticks = 0;
    while (ticks++ < 30000 && rig.sim.find(factory)->alive) {
        rig.sim.step();
    }
    // The ANCHOR: if the sim ever starts erasing the dead, this test stops testing anything.
    ASSERT_NE(rig.sim.find(factory), nullptr) << "a dead unit must still be in the sim's table";
    ASSERT_FALSE(rig.sim.find(factory)->alive) << "the enemy never killed the factory";
    const std::vector<sim::UnitId> selection{factory};
    EXPECT_EQ(menu_subject_of(rig.sim, selection), MenuSubject::None);
}

} // namespace
} // namespace protekt::app
