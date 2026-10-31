// The app layer's eyes, radar and ears follow the tank the player is DRIVING, read fresh each
// frame. The bug these pin: the camera, radar and listener kept the id from startup, so after an
// X swap the throttle drove the new tank while the view stayed on the old one.
#include "protekt/app/player_view.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
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
    rig.options.driven_unit = rig.tank_a;
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
    rig.options.driven_unit = session.driven(); // as `Game::command` sets it
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

/// The key and verb of every build row `rows` draws -- the rows under TAB and `~ Drop Off`, which
/// are digits -- in order.
std::vector<std::pair<std::string, std::string>>
build_rows_of(const std::vector<game::MenuRow>& rows) {
    std::vector<std::pair<std::string, std::string>> build;
    for (const game::MenuRow& row : rows) {
        if (row.key.size() == 1 && row.key.front() >= '1' && row.key.front() <= '9') {
            build.emplace_back(std::string{row.key}, row.verb);
        }
    }
    return build;
}

// THE FACTORY'S OWN MENU. A selected producer's menu is ITS list: the factory builds the tank and
// nothing else yet, the deployed recycler builds the seven. The old shape gave every producer the
// side's one list, so a factory offered a Harvester and a Scout it has no business building.
TEST(PlayerView, TheBuildMenuIsTheSelectedProducersOwnList) {
    sim::Scenario scenario = base_scenario();
    scenario.spawns.push_back(sim::Spawn{UnitKind::DeployedRecycler, Side::Player, 56, 48});
    Rig rig{};
    rig.sim = sim::Sim::create(scenario);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    const sim::UnitId factory = unit_id(rig.sim, 0);
    const sim::UnitId recycler = unit_id(rig.sim, 6);
    ASSERT_EQ(rig.sim.find(factory)->kind, UnitKind::Factory);
    ASSERT_EQ(rig.sim.find(recycler)->kind, UnitKind::DeployedRecycler);

    const auto menu_of = [&rig](sim::UnitId id) {
        Session session{unit_id(rig.sim, 3)};
        select_with_space(rig, session, id);
        EXPECT_EQ(session.selection().units().size(), 1U);
        EXPECT_EQ(session.selection().units().front(), id) << "the rig must select the producer";
        // 999 banked: no row is dim, so the lists are compared by what they ARE.
        game::MenuInput in{game::menu_subject_of(rig.sim, session.selection().units()),
                           game::Faction::Human, game::ArmedCommand{}, MenuTarget::None, 999};
        return build_rows_of(game::menu_rows(in));
    };

    const auto factory_list = menu_of(factory);
    const auto recycler_list = menu_of(recycler);
    EXPECT_EQ(factory_list,
              (std::vector<std::pair<std::string, std::string>>{{"1", "Mortar Bike 45"},
                                                                {"2", "MSL Scout 45"},
                                                                {"3", "Tank 55"},
                                                                {"4", "Rocket Tank 65"},
                                                                {"5", "Assault Tank 70"},
                                                                {"6", "Walker 100"},
                                                                {"7", "APC 50"},
                                                                {"8", "Bomber 75"}}))
        << "a selected factory draws the factory's own eight rows";
    ASSERT_EQ(recycler_list.size(), 7U) << "a selected deployed recycler draws the recycler's list";
    EXPECT_EQ(recycler_list.front(), (std::pair<std::string, std::string>{"1", "Scavenger 20"}));
    EXPECT_EQ(recycler_list.at(2), (std::pair<std::string, std::string>{"3", "Turret 40"}));
    EXPECT_NE(factory_list, recycler_list) << "two producers, one list: the old shape";
}

TEST(PlayerView, ASelectedFactoryAsksForTheFactoryMenuAndARecyclerForItsOwn) {
    sim::Scenario scenario = base_scenario();
    scenario.spawns.push_back(sim::Spawn{UnitKind::DeployedRecycler, Side::Player, 56, 48});
    const sim::Sim sim = sim::Sim::create(scenario);
    const std::vector<sim::UnitId> factory{unit_id(sim, 0)};
    const std::vector<sim::UnitId> recycler{unit_id(sim, 6)};
    EXPECT_EQ(game::menu_subject_of(sim, factory), MenuSubject::Factory)
        << "the factory is a producer with a menu of its own, not the deployment's";
    EXPECT_EQ(game::menu_subject_of(sim, recycler), MenuSubject::Deployment);
}

TEST(PlayerView, ASelectedScavengerAsksForTheHarvesterMenu) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 1)};
    EXPECT_EQ(game::menu_subject_of(rig.sim, selected), MenuSubject::Harvester);
}

TEST(PlayerView, ASelectedRecyclerAsksForTheRecyclerMenu) {
    sim::Scenario s = base_scenario();
    s.spawns.push_back(sim::Spawn{UnitKind::Recycler, Side::Player, 54, 41});
    Rig rig{};
    rig.sim = sim::Sim::create(s);
    const sim::UnitId recycler = unit_id(rig.sim, 6);
    ASSERT_EQ(rig.sim.find(recycler)->kind, UnitKind::Recycler);
    const std::vector<sim::UnitId> selected{recycler};
    ASSERT_EQ(game::menu_subject_of(rig.sim, selected), MenuSubject::Recycler);

    game::MenuInput in;
    in.subject = game::menu_subject_of(rig.sim, selected);
    const std::vector<game::MenuRow> rows = game::menu_rows(in);
    ASSERT_EQ(rows.size(), 6U) << "SPACE, TAB, ~, then 1 FOLLOW ME, 2 GO TO NAV, 3 DEPLOY";
    EXPECT_EQ(rows.at(3).verb, "Follow Me");
    EXPECT_EQ(rows.at(4).verb, "Go to Nav");
    EXPECT_EQ(rows.at(5).key, "3");
    EXPECT_EQ(rows.at(5).verb, "Deploy");
}

// RB1b: the recycler the skirmish now spawns, selected, is the recycler's menu on screen -- six
// rows, the third numbered one `3 DEPLOY` -- and the command row names it RECYCLER on the last
// vehicle key. It stays a vehicle in the F-row until it deploys (DEPLOY: see test_row_slots.cpp).
TEST(PlayerView, TheSkirmishRecyclerIsLabelledRecyclerAndAsksForTheSixRowMenu) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    game::RowState row;
    row.update(sim, Side::Player, 0);
    // Ten vehicles in id order (the factory sits on `2`, not under an F key): F10 is the last --
    // HAULER put the scavenger on F9.
    const game::RowSlot& group = row.slots.at(game::group_slot(10));
    ASSERT_EQ(group.units.size(), 1U);
    const std::optional<game::SlotReading> reading = game::read_slot(sim, group);
    ASSERT_TRUE(reading.has_value()) << "F10 holds the recycler";
    if (!reading.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    ASSERT_EQ(reading->kind, UnitKind::Recycler);
    EXPECT_EQ(game::kind_label(reading->kind), "RECYCLER");
    // HAULER: the player's ten vehicles fill the row's ten group keys exactly; F10 is the last.
    EXPECT_EQ(game::group_slot(10) + 1, game::kRowSlots) << "F10 is the row's last group key";

    const std::vector<sim::UnitId> selected{group.units.front()};
    game::MenuInput in;
    in.subject = game::menu_subject_of(sim, selected);
    ASSERT_EQ(in.subject, game::MenuSubject::Recycler);
    const std::vector<game::MenuRow> rows = game::menu_rows(in);
    ASSERT_EQ(rows.size(), 6U) << "SPACE, TAB, ~, then 1 FOLLOW ME, 2 GO TO NAV, 3 DEPLOY";
    EXPECT_EQ(rows.at(5).key, "3");
    EXPECT_EQ(rows.at(5).verb, "Deploy");
}

TEST(PlayerView, AnArmedDeployReadsSpaceDeployAndTheDistanceToTheGroundUnderTheReticle) {
    // `un-deployed recycler menu -> 3`, as the frame draws it: SPACE DEPLOY 68m.
    sim::Scenario s = base_scenario();
    s.spawns.push_back(sim::Spawn{UnitKind::Recycler, Side::Player, 54, 41});
    Rig rig{};
    rig.sim = sim::Sim::create(s);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    const sim::UnitId recycler = unit_id(rig.sim, 6);
    Session session{unit_id(rig.sim, 3)};
    select_with_space(rig, session, recycler);
    ASSERT_FALSE(menu_for(rig.sim, session, rig.options, false).empty());
    EXPECT_TRUE(menu_for(rig.sim, session, rig.options, false).front().right.empty())
        << "nothing armed: no distance";
    input::InputFrame three;
    three.keys.push_back(input::KeyEvent{input::kKey1 + 2, true, false});
    std::vector<sim::Order> out;
    session.frame(three, rig.sim, rig.options, out);
    ASSERT_TRUE(session.deploy_armed());
    const std::vector<game::MenuRow> rows = menu_for(rig.sim, session, rig.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE");
    EXPECT_EQ(rows.front().verb, "Deploy");
    const game::NavAim aim = game::nav_aim(rig.sim, rig.options);
    ASSERT_TRUE(aim.on_ground());
    EXPECT_EQ(rows.front().right, game::metres_text(aim.metres))
        << "the live distance, at the right";
}

TEST(PlayerView, ASelectedTurretAsksForTheTurretMenu) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 2)};
    EXPECT_EQ(game::menu_subject_of(rig.sim, selected), MenuSubject::Turret);
}

TEST(PlayerView, ATankSquadAsksForTheVehicleMenu) {
    // The 2026-10-26 captures measured ONE ten-row table shared by a tank squad and a lone scout;
    // the menu is drawn for a selected vehicle now, not invented for it.
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 3), unit_id(rig.sim, 4)};
    EXPECT_EQ(game::menu_subject_of(rig.sim, selected), MenuSubject::Vehicle);
}

TEST(PlayerView, TwoKindsSelectedAtOnceAskForNoMenu) {
    const Rig rig = make_base_rig();
    const std::vector<sim::UnitId> selected{unit_id(rig.sim, 0), unit_id(rig.sim, 1)};
    EXPECT_EQ(game::menu_subject_of(rig.sim, selected), MenuSubject::None)
        << "there is one subject per menu";
}

TEST(PlayerView, NothingSelectedAsksForNoMenu) {
    const Rig rig = make_base_rig();
    EXPECT_EQ(game::menu_subject_of(rig.sim, std::span<const sim::UnitId>{}), MenuSubject::None);
    const std::vector<sim::UnitId> unknown{sim::UnitId{9999}};
    EXPECT_EQ(game::menu_subject_of(rig.sim, unknown), MenuSubject::None)
        << "an id that is not in the sim is not selected";
}

TEST(PlayerView, TheCursorOverYourOwnCraftOffersSelect) {
    Rig rig = make_base_rig();
    const Session session{unit_id(rig.sim, 3)};
    aim_at(rig, session, unit_id(rig.sim, 4));
    // `aim_at` above engaged the aim, but it does so through a reference parameter, so the
    // analyzer cannot see the assignment and treats every later read as unchecked -- including
    // through value(), which is safe at run time and invisible to it. So bind the optional,
    // check it in the form the analyzer follows, and read through the binding. The other test
    // in this file needs none of this: it assigns its own aim directly, two lines above its read.
    const auto& aim = rig.options.aim;
    ASSERT_TRUE(aim.has_value()) << "the rig must be aimed";
    if (!aim.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    ASSERT_EQ(game::pick(rig.sim, rig.options, aim->x, aim->y).unit, unit_id(rig.sim, 4))
        << "the rig must be aimed at the tank";
    EXPECT_EQ(menu_target_of(rig.sim, rig.options), MenuTarget::Selectable);
}

TEST(PlayerView, TheCursorOverAnEnemyNamesNoSelect) {
    Rig rig = make_base_rig();
    const Session session{unit_id(rig.sim, 3)};
    aim_at(rig, session, unit_id(rig.sim, 5));
    // `aim_at` above engaged the aim, but it does so through a reference parameter, so the
    // analyzer cannot see the assignment and treats every later read as unchecked -- including
    // through value(), which is safe at run time and invisible to it. So bind the optional,
    // check it in the form the analyzer follows, and read through the binding. The other test
    // in this file needs none of this: it assigns its own aim directly, two lines above its read.
    const auto& aim = rig.options.aim;
    ASSERT_TRUE(aim.has_value()) << "the rig must be aimed";
    if (!aim.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    ASSERT_EQ(game::pick(rig.sim, rig.options, aim->x, aim->y).unit, unit_id(rig.sim, 5))
        << "the rig must be aimed at the enemy";
    EXPECT_EQ(menu_target_of(rig.sim, rig.options), MenuTarget::Hostile);
}

TEST(PlayerView, TheCursorOverBareGroundNamesNothing) {
    Rig rig = make_base_rig();
    rig.options.driven_unit = unit_id(rig.sim, 3); // as `Game::command` sets it
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
    // The deployed recycler's seven rows are weighed; the hauler banks into the factory.
    sim::Scenario scenario = base_scenario();
    scenario.spawns.push_back(sim::Spawn{UnitKind::DeployedRecycler, Side::Player, 56, 48});
    Rig rig{};
    rig.sim = sim::Sim::create(scenario);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    Session session{unit_id(rig.sim, 3)};
    select_with_space(rig, session, unit_id(rig.sim, 6));
    ASSERT_EQ(game::menu_subject_of(rig.sim, session.selection().units()), MenuSubject::Deployment);
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
    EXPECT_EQ(game::menu_subject_of(rig.sim, selection), MenuSubject::None);
}

// HAULER: the skirmish's own scavenger -- the vehicle that makes the economy playable -- is a
// harvester as far as the game layer is concerned. F9 is its key: the recycler moved to F10.
TEST(PlayerView, TheSkirmishScavengerIsLabelledScavengerAndAsksForTheHarvesterMenu) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    game::RowState row;
    row.update(sim, Side::Player, 0);
    const game::RowSlot& group = row.slots.at(game::group_slot(9));
    ASSERT_EQ(group.units.size(), 1U) << "F9 holds the skirmish's scavenger";
    const std::optional<game::SlotReading> reading = game::read_slot(sim, group);
    ASSERT_TRUE(reading.has_value());
    if (!reading.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    ASSERT_EQ(reading->kind, UnitKind::Scavenger);
    EXPECT_EQ(game::kind_label(reading->kind), "SCAVENGER");
    const std::vector<sim::UnitId> selected{group.units.front()};
    EXPECT_EQ(game::menu_subject_of(sim, selected), MenuSubject::Harvester);
}

TEST(CameraYawed, TurnsOnlyTheYawByHundredthsOfARadianAndWrapsAtPi) {
    game::Camera camera;
    camera.x = 3.0F;
    camera.yaw = 0.5F;
    camera.pitch = 0.25F;
    const game::Camera turned = camera_yawed(camera, 122);
    EXPECT_NEAR(turned.yaw, 1.72F, 1e-4F);
    EXPECT_EQ(turned.pitch, camera.pitch);
    EXPECT_EQ(turned.x, camera.x);
    EXPECT_NEAR(camera_yawed(camera, -243).yaw, 0.5F - 2.43F, 1e-4F);
    // 0.5 + 3.0 is past pi: it comes back round to the other side.
    EXPECT_NEAR(camera_yawed(camera, 300).yaw, 3.5F - 2.0F * 3.14159265F, 1e-4F);
    EXPECT_EQ(camera_yawed(camera, 0).yaw, camera.yaw);
}

// ------------------------------------------------------------------------ the eye on the barrel

/// What the eye was before it was derived: 3.0 m above the hull, the number `camera_for` used to
/// carry. Here only so the test can show the old eye FAILS the check the new one passes.
constexpr float kOldEyeAboveHull = 3.0F;
/// How far inside the half field of view the muzzle tip must land. Harri's 1 m lift puts it at
/// 26.6 of the 30 degrees, so the margin is 2 degrees: a small tune keeps passing and a lift past
/// the frame's edge (about 1.15 m) does not.
constexpr float kFrameMarginRadians = 2.0F * 3.14159265F / 180.0F;

/// The driven tank, the hull's height at rest, and the muzzle tip at level pitch for a camera: the
/// tip is `kTankBarrelTip` along the view's yaw from the eye and on the barrel's own axis.
struct MuzzleView {
    game::Camera camera;
    float hull_z{};
    std::array<float, 3> tip{};
};

MuzzleView muzzle_view(Rig& rig, const Session& session) {
    MuzzleView view;
    view.camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    view.camera.pitch = 0.0F; // LEVEL: the session starts looking a little down
    const sim::Unit* unit = rig.sim.find(session.driven());
    view.hull_z = game::ground_metres(kSeed, unit->x, unit->y) + game::kRideHeightMetres;
    const std::array<float, 3> forward = math::basis_from(view.camera.yaw, 0.0F).forward;
    view.tip = {view.camera.x + (forward.at(0) * game::kTankBarrelTip),
                view.hull_z + game::kTankBarrelAxisMetres,
                view.camera.z + (forward.at(2) * game::kTankBarrelTip)};
    return view;
}

/// The muzzle tip's angle below the view axis, from the camera's own height and the tip's: the
/// geometry, with no field of view in it.
float tip_angle_below_axis(const MuzzleView& view, float eye_y) {
    const float run = std::hypot(view.tip.at(0) - view.camera.x, view.tip.at(2) - view.camera.z);
    return std::atan2(eye_y - view.tip.at(1), run);
}

TEST(EyeOnTheBarrel, TheEyeStandsOnTheBarrelsAxisPlusTheDocumentedLift) {
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    const MuzzleView view = muzzle_view(rig, session);
    // Against the barrel's OWN constants, not a number typed here: raise the barrel's axis and this
    // fails instead of the eye and the gun drifting apart.
    const float axis
        = game::kTankTurretBase + ((game::kTankBarrelBottom + game::kTankBarrelTop) * 0.5F);
    EXPECT_NEAR(view.camera.y - view.hull_z, axis + kEyeAboveBarrelMetres, 1.0e-5F);
    EXPECT_NEAR(view.camera.y, view.tip.at(1) + kEyeAboveBarrelMetres, 1.0e-5F)
        << "at level pitch the eye is the tip's height, plus the lift";
}

TEST(EyeOnTheBarrel, TheMuzzleTipIsInFrameAtLevelPitchAndTheOldEyeHadItOutside) {
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    const MuzzleView view = muzzle_view(rig, session);
    const float half_fov = math::kFovRadians * 0.5F;

    // A vacuous pass would be a tip that was never placed ahead of the eye.
    const float run = std::hypot(view.tip.at(0) - view.camera.x, view.tip.at(2) - view.camera.z);
    ASSERT_GT(run, 1.0F) << "the tip must have been computed, well ahead of the eye";

    const float now = tip_angle_below_axis(view, view.camera.y);
    const float before = tip_angle_below_axis(view, view.hull_z + kOldEyeAboveHull);
    EXPECT_LT(now, half_fov - kFrameMarginRadians) << "the tip is not clear of the frame's edge";
    EXPECT_GT(before, half_fov)
        << "the old eye must put the tip outside the frame, or this proves nothing";

    // The same answer from the game's own projection, which is what reads the camera's field of
    // view: the tip lands on the screen now, and below its bottom edge with the old eye.
    const game::Projected seen
        = game::project(view.camera, kExtent, view.tip.at(0), view.tip.at(1), view.tip.at(2));
    EXPECT_GT(seen.depth, math::kNearPlane);
    EXPECT_GE(seen.y, 0.0F);
    EXPECT_LE(seen.y, static_cast<float>(kExtent.height));
    game::Camera old_eye = view.camera;
    old_eye.y = view.hull_z + kOldEyeAboveHull;
    const game::Projected missed
        = game::project(old_eye, kExtent, view.tip.at(0), view.tip.at(1), view.tip.at(2));
    EXPECT_GT(missed.y, static_cast<float>(kExtent.height))
        << "the old eye saw the tip below the screen";
    // The pixel row agrees with the angle: the projection is reading the real half field of view.
    const float centre_row = static_cast<float>(kExtent.height) * 0.5F;
    EXPECT_NEAR(seen.y, centre_row + (centre_row * std::tan(now) / std::tan(half_fov)), 0.5F);
}

TEST(EyeOnTheBarrel, TheViewIsStillTheBarrelsOwnYawAndPitch) {
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    EXPECT_EQ(camera.yaw, session.view_yaw());
    EXPECT_EQ(camera.pitch, session.view_pitch());
    // The screen's centre looks along `basis_from(yaw, pitch).forward`, from the new eye.
    const std::array<float, 3> forward = math::basis_from(camera.yaw, camera.pitch).forward;
    constexpr float kAhead = 10.0F;
    const game::Projected centre
        = game::project(camera, kExtent, camera.x + (forward.at(0) * kAhead),
                        camera.y + (forward.at(1) * kAhead), camera.z + (forward.at(2) * kAhead));
    ASSERT_GT(centre.depth, math::kNearPlane);
    EXPECT_NEAR(centre.x, static_cast<float>(kExtent.width) * 0.5F, 0.5F);
    EXPECT_NEAR(centre.y, static_cast<float>(kExtent.height) * 0.5F, 0.5F);
}

TEST(EyeOnTheBarrel, TheDriversOwnTankIsNotWhatTheViewPicks) {
    // The eye is at or just over the driven tank's own pick box (1.8 m over the ground; the eye at
    // the 1 m lift is 2.3 m), and a steep ray down to a lower target clips its corner: the driven
    // hull is never what the view names.
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    rig.options.driven_unit = rig.tank_a;
    rig.options.camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    const sim::Unit* own = rig.sim.find(rig.tank_a);
    ASSERT_NE(own, nullptr);
    ASSERT_GT(rig.options.camera.y, game::ground_metres(kSeed, own->x, own->y));
    const game::Pick picked
        = game::pick(rig.sim, rig.options, static_cast<float>(kExtent.width) * 0.5F,
                     static_cast<float>(kExtent.height) * 0.5F);
    EXPECT_NE(picked.unit, rig.tank_a);
}

// ------------------------------------------------------------------- the three camera positions

constexpr std::array<game::CameraPosition, game::kCameraPositionCount> kPositions{
    game::CameraPosition::Barrel, game::CameraPosition::Chase, game::CameraPosition::HighChase};

/// One frame with only V pressed.
void press_camera_key(Rig& rig, Session& session) {
    input::InputFrame frame;
    frame.keys.push_back(input::KeyEvent{input::kKeyV, true, false});
    std::vector<sim::Order> out;
    session.frame(frame, rig.sim, rig.options, out);
}

/// The camera at each position, the session rotated by V presses rather than set.
std::array<game::Camera, game::kCameraPositionCount> cameras_at_each_position(Rig& rig) {
    Session session{rig.tank_a};
    std::array<game::Camera, game::kCameraPositionCount> cameras{};
    for (game::Camera& camera : cameras) {
        camera = camera_for(rig.sim, session, kSeed, std::nullopt);
        camera.pitch = 0.0F; // LEVEL, as `muzzle_view` measures
        press_camera_key(rig, session);
    }
    return cameras;
}

/// The muzzle tip's angle off the view axis, in radians, from `camera`: the tip stands
/// `kTankBarrelTip` along the view's yaw from the HULL's centre (not the eye's), on the barrel's
/// axis. Straight geometry, no field of view in it.
float tip_angle_from(const MuzzleView& hull, const game::Camera& camera) {
    return tip_angle_below_axis(MuzzleView{camera, hull.hull_z, hull.tip}, camera.y);
}

TEST(CameraPositions, TheEyeIsTheBarrelsAxisPlusTheLiftWhateverTheTwoAreTuneTo) {
    // The composition, not a typed number: swap the two constants or retype one and this fails.
    EXPECT_EQ(kEyeAboveHullMetres, game::kTankBarrelAxisMetres + kEyeAboveBarrelMetres);
    EXPECT_EQ(eye_placement(game::CameraPosition::Barrel).above_hull_metres, kEyeAboveHullMetres);
    EXPECT_EQ(eye_placement(game::CameraPosition::Chase).above_hull_metres, kEyeAboveHullMetres);
    EXPECT_EQ(eye_placement(game::CameraPosition::HighChase).above_hull_metres,
              game::kTankBarrelAxisMetres + kHighChaseLiftMetres);
    EXPECT_EQ(eye_placement(game::CameraPosition::Barrel).behind_metres, 0.0F);
}

TEST(CameraPositions, EachPositionPutsTheEyeSomewhereDistinct) {
    Rig rig = make_rig();
    const auto cameras = cameras_at_each_position(rig);
    const auto separation = [](const game::Camera& a, const game::Camera& b) {
        return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
    };
    EXPECT_GT(separation(cameras.at(0), cameras.at(1)), 1.0F);
    EXPECT_GT(separation(cameras.at(1), cameras.at(2)), 1.0F);
    EXPECT_GT(separation(cameras.at(0), cameras.at(2)), 1.0F);
    // Guard against a vacuous pass: the heights differ, so this is not three copies of one eye.
    EXPECT_FALSE(cameras.at(0).y == cameras.at(1).y && cameras.at(1).y == cameras.at(2).y);
    EXPECT_GT(cameras.at(2).y, cameras.at(1).y + 1.0F) << "the third is well above the second";
}

TEST(CameraPositions, TheChaseEyesStandBehindTheHullAlongTheViewAndKeepTheAim) {
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    const auto cameras = cameras_at_each_position(rig);
    const sim::Unit* own = rig.sim.find(rig.tank_a);
    const float hull_x = static_cast<float>(own->x.raw) / 65536.0F * game::kMetresPerTile;
    const float hull_z = static_cast<float>(own->y.raw) / 65536.0F * game::kMetresPerTile;
    const std::array<float, 3> forward = math::basis_from(session.view_yaw(), 0.0F).forward;
    for (std::size_t i = 0; i < kPositions.size(); ++i) {
        const float behind = eye_placement(kPositions.at(i)).behind_metres;
        // Along the view, so the offset's component on the view is the setback, and across it,
        // none.
        const float along = ((cameras.at(i).x - hull_x) * forward.at(0))
                            + ((cameras.at(i).z - hull_z) * forward.at(2));
        EXPECT_NEAR(along, -behind, 1.0e-4F);
        EXPECT_NEAR(std::hypot(cameras.at(i).x - hull_x, cameras.at(i).z - hull_z), behind,
                    1.0e-4F);
        EXPECT_EQ(cameras.at(i).yaw, session.view_yaw());
    }
    EXPECT_GT(eye_placement(game::CameraPosition::Chase).behind_metres, 0.0F);
    EXPECT_GT(eye_placement(game::CameraPosition::HighChase).behind_metres,
              eye_placement(game::CameraPosition::Chase).behind_metres);
}

TEST(CameraPositions, VCyclesInOrderAndThreePressesComeBackToTheFirst) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    ASSERT_EQ(session.camera_position(), game::CameraPosition::Barrel);
    press_camera_key(rig, session);
    EXPECT_EQ(session.camera_position(), game::CameraPosition::Chase);
    press_camera_key(rig, session);
    EXPECT_EQ(session.camera_position(), game::CameraPosition::HighChase)
        << "two presses land on the third position, not the second";
    press_camera_key(rig, session);
    EXPECT_EQ(session.camera_position(), game::CameraPosition::Barrel);
}

TEST(CameraPositions, OnlyABareVPressRotatesTheCamera) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    input::InputFrame frame;
    frame.keys.push_back(input::KeyEvent{input::kKeyV, false, false});      // a release
    frame.keys.push_back(input::KeyEvent{input::kKeyV, true, false, true}); // Ctrl+V
    frame.keys.push_back(input::KeyEvent{input::kKeyC, true, false});       // C is calibration
    std::vector<sim::Order> out;
    session.frame(frame, rig.sim, rig.options, out);
    EXPECT_EQ(session.camera_position(), game::CameraPosition::Barrel);
}

TEST(CameraPositions, TheRotationDoesNotChangeTheAim) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    const float yaw = session.view_yaw();
    const float pitch = session.view_pitch();
    for (std::size_t i = 0; i < game::kCameraPositionCount; ++i) {
        const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
        EXPECT_EQ(camera.yaw, yaw);
        EXPECT_EQ(camera.pitch, pitch);
        press_camera_key(rig, session);
    }
    EXPECT_EQ(session.camera_position(), game::CameraPosition::Barrel);
    EXPECT_EQ(session.view_yaw(), yaw);
    EXPECT_EQ(session.view_pitch(), pitch);
}

TEST(CameraPositions, TheMuzzleTipIsInFrameAtThe1mBarrelPositionAndAtEveryOther) {
    Rig rig = make_rig();
    const Session base{rig.tank_a};
    const MuzzleView hull = muzzle_view(rig, base);
    const auto cameras = cameras_at_each_position(rig);
    const float half_fov = math::kFovRadians * 0.5F;
    ASSERT_NEAR(kEyeAboveBarrelMetres, 1.0F, 1.0e-6F) << "this test is about Harri's 1 m";
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const float angle = tip_angle_from(hull, cameras.at(i));
        EXPECT_GT(angle, 0.0F) << "position " << i << ": the tip is below the view axis";
        EXPECT_LT(angle, half_fov - kFrameMarginRadians) << "position " << i;
        // And through the game's own projection: the tip lands on the screen.
        const game::Projected seen
            = game::project(cameras.at(i), kExtent, hull.tip.at(0), hull.tip.at(1), hull.tip.at(2));
        EXPECT_GT(seen.depth, math::kNearPlane) << "position " << i;
        EXPECT_GE(seen.y, 0.0F) << "position " << i;
        EXPECT_LE(seen.y, static_cast<float>(kExtent.height)) << "position " << i;
    }
    // At the barrel position the angle is atan(lift / reach): the geometry, not a typed degree.
    EXPECT_NEAR(tip_angle_from(hull, cameras.at(0)),
                std::atan2(kEyeAboveBarrelMetres, game::kTankBarrelTip), 1.0e-4F);
    // The vacuous guard: a lift past the frame's edge FAILS the same check.
    game::Camera too_high = cameras.at(0);
    too_high.y = hull.hull_z + game::kTankBarrelAxisMetres + 1.5F;
    EXPECT_GT(tip_angle_from(hull, too_high), half_fov);
}

TEST(CameraPositions, TheDrivenHullIsNotWhatAChaseViewPicks) {
    // From behind, the ray passes through the driven hull's own box: it must not name it.
    Rig rig = make_rig();
    Session session{rig.tank_a};
    press_camera_key(rig, session);
    rig.options.driven_unit = rig.tank_a;
    rig.options.camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    const sim::Unit* own = rig.sim.find(rig.tank_a);
    // Aim the ray at the hull: from the eye to the hull's middle.
    const float hull_x = static_cast<float>(own->x.raw) / 65536.0F * game::kMetresPerTile;
    const float hull_z = static_cast<float>(own->y.raw) / 65536.0F * game::kMetresPerTile;
    const float hull_y
        = game::ground_metres(kSeed, own->x, own->y) + game::kRideHeightMetres + 0.5F;
    const game::Projected hull = game::project(rig.options.camera, kExtent, hull_x, hull_y, hull_z);
    ASSERT_GT(hull.depth, math::kNearPlane) << "the hull must be in front of the chase eye";
    ASSERT_GE(hull.x, 0.0F);
    ASSERT_LE(hull.x, static_cast<float>(kExtent.width));
    EXPECT_NE(game::pick(rig.sim, rig.options, hull.x, hull.y).unit, rig.tank_a);
    rig.options.driven_unit = sim::UnitId{};
    EXPECT_EQ(game::pick(rig.sim, rig.options, hull.x, hull.y).unit, rig.tank_a)
        << "without the rule the ray does hit the hull, or this proves nothing";
}

} // namespace
} // namespace protekt::app
