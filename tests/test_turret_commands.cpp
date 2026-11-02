// TURRET COMMANDS and the PLAYER'S SCAVENGER DEPLOY in the game layer (Harri, 2026-11-01).
//
//  * SCAVENGER: "select a single scavenger, point at the pool within the narrow arc, and hit SPACE
//    Deploy. Deploy is only shown when a scavenger is selected and you're pointing approximately at
//    a pool." One scavenger, a pool under the pointing cone (the AI's own 4 degrees), a prompt that
//    exists only there.
//  * TURRETS: `1 Follow Me`, `6 Defend`, `SPACE Deploy` at the spot pointed at, `7 Undeploy`, and
//  the
//    vehicle's usual rows.
// One test per rule, each isolating its own number, so a red names the rule that broke.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/cmd/command.hpp"
#include "protekt/game/assist.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

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
constexpr float kCone = static_cast<float>(sim::kPointingConeDegrees);

constexpr int kPoolX = 40;
constexpr int kPoolY = 60;

struct Field {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;
    UnitId scav_a;
    UnitId scav_b;
    UnitId turret_a;
    UnitId turret_b;
    UnitId enemy;
};

/// The camera stands 20 tiles short of the pool's middle at the height of the ground there,
/// looking straight at it (yaw 0 is +Z), so a yaw of `d` puts the pool exactly `d` off the
/// crosshair with the ray level: the angle is the yaw. Two pools, so "the nearer to the ray wins"
/// has something to choose between; the second is far to the east, out of any cone.
[[nodiscard]] Field make_field() {
    Field f{};
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.pools = {sim::Pool{kPoolX, kPoolY}, sim::Pool{kPoolX + 30, kPoolY}};
    scenario.spawns = {{UnitKind::Tank, Side::Player, kPoolX - 4, kPoolY - 16},
                       {UnitKind::Scavenger, Side::Player, 36, 50},
                       {UnitKind::Scavenger, Side::Player, 37, 50},
                       {UnitKind::Turret, Side::Player, 34, 50},
                       {UnitKind::Turret, Side::Player, 35, 50},
                       {UnitKind::Tank, Side::Enemy, 60, 62}};
    f.sim = sim::Sim::create(scenario);
    const auto& units = f.sim.units();
    f.driven = units[0].id;
    f.scav_a = units[1].id;
    f.scav_b = units[2].id;
    f.turret_a = units[3].id;
    f.turret_b = units[4].id;
    f.enemy = units[5].id;
    f.options.seed = kSeed;
    f.options.extent = kExtent;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    f.options.camera = Camera{
        (static_cast<float>(kPoolX) + 0.5F) * kMetresPerTile,
        ground_metres(kSeed, static_cast<float>(kPoolX) + 0.5F, static_cast<float>(kPoolY) + 0.5F),
        (static_cast<float>(kPoolY) + 0.5F - 20.0F) * kMetresPerTile, 0.0F, 0.0F};
    f.options.aim = Aim{640.0F, 360.0F};
    f.options.row = &f.row;
    f.row.update(f.sim, Side::Player, 0);
    return f;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] std::optional<DeployOffer> offered(const Field& f, std::vector<UnitId> selected) {
    return deploy_offer(f.sim, f.options, selected, f.driven);
}

/// Look at the ground instead of at the pool: raised well clear of the terrain (the pool rig's
/// level camera can sit under a rise) and pitched down, so the ray meets bare terrain ahead, with
/// nothing standing there.
void look_at_ground(Field& f) {
    f.options.camera.y = ground_metres(kSeed, f.options.camera.x / kMetresPerTile,
                                       f.options.camera.z / kMetresPerTile)
                         + 30.0F;
    f.options.camera.pitch = -0.6F;
}

/// Select `id` through its F key, the way the player does. The slot is whatever the row says.
[[nodiscard]] Interaction select(Field& f, UnitId id) {
    Interaction player{f.driven};
    for (std::size_t slot = kRowFirstGroup; slot < kRowSlots; ++slot) {
        const auto& units = f.row.slots.at(slot).units;
        if (std::find(units.begin(), units.end(), id) != units.end()) {
            player.handle(
                key_frame(input::kKeyF1 + static_cast<std::int32_t>(slot - kRowFirstGroup)), f.sim,
                f.options);
            break;
        }
    }
    return player;
}

// ============================================================ the scavenger's prompt (SPACE
// Deploy)

TEST(ScavengerDeployPrompt, TheConeIsTheAIsConeAndNoSecondArcExists) {
    EXPECT_EQ(kAssistConeDegrees, static_cast<float>(sim::kPointingConeDegrees))
        << "the player's arc IS the one the AI's deploy points with";
    EXPECT_EQ(sim::kPointingConeDegrees, 4);
}

TEST(ScavengerDeployPrompt, OneScavengerPointingAtAPoolIsOffered) {
    Field f = make_field();
    const std::optional<DeployOffer> offer = offered(f, {f.scav_a});
    ASSERT_TRUE(offer.has_value());
    EXPECT_EQ(offer.value_or(DeployOffer{}).site, DeploySite::Pool);
    EXPECT_EQ(offer.value_or(DeployOffer{}).tile_x, kPoolX);
    EXPECT_EQ(offer.value_or(DeployOffer{}).tile_y, kPoolY);
}

TEST(ScavengerDeployPrompt, AGroupOfScavengersIsNeverOfferedIt) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {f.scav_a, f.scav_b}).has_value());
}

TEST(ScavengerDeployPrompt, ScavengerWithNoPoolUnderTheConeIsOfferedNothing) {
    Field f = make_field();
    f.options.camera.yaw = (kCone + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(offered(f, {f.scav_a}).has_value()) << "just outside the cone";
    f.options.camera.yaw = -(kCone + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(offered(f, {f.scav_a}).has_value()) << "and on the other side";
}

TEST(ScavengerDeployPrompt, AnEdgeInsideTheConeIsOfferedOnBothSides) {
    Field f = make_field();
    f.options.camera.yaw = (kCone - 0.5F) * kRadiansPerDegree;
    EXPECT_TRUE(offered(f, {f.scav_a}).has_value());
    f.options.camera.yaw = -(kCone - 0.5F) * kRadiansPerDegree;
    EXPECT_TRUE(offered(f, {f.scav_a}).has_value());
}

TEST(ScavengerDeployPrompt, ThePoolHasToBeInTheRadarsReach) {
    Field f = make_field();
    // The viewer is taken 90 tiles away from the pool: beyond a tank's 40-tile radar.
    f.sim = [&] {
        sim::Scenario scenario;
        scenario.seed = kSeed;
        scenario.pools = {sim::Pool{kPoolX, kPoolY}};
        scenario.spawns = {{UnitKind::Tank, Side::Player, kPoolX, kPoolY - 90},
                           {UnitKind::Scavenger, Side::Player, 36, 50}};
        return sim::Sim::create(scenario);
    }();
    f.driven = f.sim.units()[0].id;
    f.scav_a = f.sim.units()[1].id;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    EXPECT_FALSE(offered(f, {f.scav_a}).has_value()) << "the player cannot see it to point at it";
}

TEST(ScavengerDeployPrompt, APoolAScavengerHoldsIsNotOfferedAgain) {
    Field f = make_field();
    // The sim's own rule: one deployed scavenger per pool, either side's.
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.pools = {sim::Pool{kPoolX, kPoolY}};
    scenario.spawns = {{UnitKind::Tank, Side::Player, kPoolX - 4, kPoolY - 16},
                       {UnitKind::Scavenger, Side::Player, 36, 50},
                       {UnitKind::DeployedScavenger, Side::Enemy, kPoolX, kPoolY}};
    f.sim = sim::Sim::create(scenario);
    f.driven = f.sim.units()[0].id;
    f.scav_a = f.sim.units()[1].id;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    EXPECT_FALSE(offered(f, {f.scav_a}).has_value());
}

TEST(ScavengerDeployPrompt, TheDrivenScavengerCannotBeOrderedSoItIsNotOffered) {
    Field f = make_field();
    f.driven = f.scav_a;
    EXPECT_FALSE(offered(f, {f.scav_a}).has_value());
}

TEST(ScavengerDeployPrompt, WithNoCrosshairThereIsNothingToPointWith) {
    Field f = make_field();
    f.options.aim.reset();
    EXPECT_FALSE(offered(f, {f.scav_a}).has_value());
}

TEST(ScavengerDeployPrompt, NothingElseSelectedIsOfferedThePoolDeploy) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {f.driven}).has_value());
    EXPECT_FALSE(offered(f, {}).has_value());
    // A tank cannot deploy onto a pool, so pointing a tank at one offers nothing.
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.pools = {sim::Pool{kPoolX, kPoolY}};
    scenario.spawns = {{UnitKind::Tank, Side::Player, kPoolX - 4, kPoolY - 16},
                       {UnitKind::Tank, Side::Player, 36, 50}};
    f.sim = sim::Sim::create(scenario);
    f.driven = f.sim.units()[0].id;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    EXPECT_FALSE(offered(f, {f.sim.units()[1].id}).has_value());
}

TEST(ScavengerDeployPrompt, TheNearerPoolToTheRayWins) {
    Field f = make_field();
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.pools = {sim::Pool{kPoolX + 2, kPoolY}, sim::Pool{kPoolX, kPoolY}};
    scenario.spawns = {{UnitKind::Tank, Side::Player, kPoolX - 4, kPoolY - 16},
                       {UnitKind::Scavenger, Side::Player, 36, 50}};
    f.sim = sim::Sim::create(scenario);
    f.driven = f.sim.units()[0].id;
    f.scav_a = f.sim.units()[1].id;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    const std::optional<DeployOffer> offer = offered(f, {f.scav_a});
    ASSERT_TRUE(offer.has_value());
    EXPECT_EQ(offer.value_or(DeployOffer{}).tile_x, kPoolX)
        << "the dead-ahead pool, though it is listed second";
}

TEST(ScavengerDeployPrompt, TheMenuShowsDeployOnlyInThatContext) {
    Field f = make_field();
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1 + 1), f.sim, f.options, orders); // F2: scavenger A
    ASSERT_EQ(session.selection().size(), 1U);

    std::vector<MenuRow> rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE");
    EXPECT_EQ(rows.front().verb, "Deploy") << "a scavenger selected, a pool under the cone";

    f.options.camera.yaw = (kCone + 0.5F) * kRadiansPerDegree;
    rows = app::menu_for(f.sim, session, f.options, false);
    for (const MenuRow& row : rows) {
        EXPECT_NE(row.verb, "Deploy")
            << "no pool under the cone: no Deploy anywhere, key " << row.key;
    }
}

TEST(ScavengerDeployPrompt, AGroupGetsNoDeployPromptInTheMenu) {
    Field f = make_field();
    ASSERT_EQ(f.row.move_into(f.sim, std::vector<UnitId>{f.scav_a, f.scav_b}, 2), RowMove::Moved);
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1 + 1), f.sim, f.options, orders);
    ASSERT_EQ(session.selection().size(), 2U);
    for (const MenuRow& row : app::menu_for(f.sim, session, f.options, false)) {
        EXPECT_NE(row.verb, "Deploy") << "key " << row.key;
    }
}

TEST(ScavengerDeployPrompt, SpaceSendsTheScavengerToThePoolAndEndsTheCommand) {
    Field f = make_field();
    Interaction player = select(f, f.scav_a);
    ASSERT_EQ(player.selection().size(), 1U);

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, f.scav_a);
    ASSERT_EQ(orders.front().kind(), sim::OrderKind::Deploy);
    EXPECT_EQ(std::get<sim::orders::Deploy>(orders.front().payload).tile_x, kPoolX);
    EXPECT_EQ(std::get<sim::orders::Deploy>(orders.front().payload).tile_y, kPoolY);
    EXPECT_TRUE(player.selection().empty()) << "a finished command clears the selection";
}

TEST(ScavengerDeployPrompt, SpaceOutsideTheConeOrdersNothing) {
    Field f = make_field();
    Interaction player = select(f, f.scav_a);
    f.options.camera.yaw = (kCone + 0.5F) * kRadiansPerDegree;
    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    EXPECT_TRUE(player.take_orders().empty()) << "what the menu does not offer, SPACE does not do";
    EXPECT_EQ(player.selection().size(), 1U);
}

TEST(ScavengerDeployPrompt, TheOrderTheKeyGivesIsOneTheSimTakesAndTheScavengerThenDeploys) {
    Field f = make_field();
    Interaction player = select(f, f.scav_a);
    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    for (const sim::Order& order : player.take_orders()) {
        f.sim.enqueue(order);
    }
    ASSERT_EQ(f.sim.command_log().size(), 1U) << "the sim accepted it";
    for (std::uint32_t i = 0; i < 400; ++i) {
        f.sim.step();
    }
    EXPECT_EQ(f.sim.find(f.scav_a)->kind, UnitKind::DeployedScavenger);
}

// ====================================================================== the turret's commands

TEST(TurretCommands, TurretsPointedAtBareGroundAreOfferedTheDeployThere) {
    Field f = make_field();
    look_at_ground(f);
    const Pick picked = pick(f.sim, f.options, 640.0F, 360.0F);
    ASSERT_TRUE(picked.has_tile());
    ASSERT_FALSE(picked.has_unit());
    const std::optional<DeployOffer> one = offered(f, {f.turret_a});
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one.value_or(DeployOffer{}).site, DeploySite::Spot);
    EXPECT_EQ(one.value_or(DeployOffer{}).tile_x, picked.tile_x);
    EXPECT_EQ(one.value_or(DeployOffer{}).tile_y, picked.tile_y);
    EXPECT_TRUE(offered(f, {f.turret_a, f.turret_b}).has_value()) << "a group of turrets too";
}

TEST(TurretCommands, NoGroundNoOfferAndNotForANonTurret) {
    Field f = make_field();
    look_at_ground(f);
    f.options.camera.pitch = 0.5F; // looking up at the sky, from well above the terrain
    ASSERT_FALSE(pick(f.sim, f.options, 640.0F, 360.0F).has_tile());
    EXPECT_FALSE(offered(f, {f.turret_a}).has_value()) << "the sky is not ground";
    look_at_ground(f);
    EXPECT_FALSE(offered(f, {f.turret_a, f.scav_a}).has_value()) << "a scavenger among them";
    EXPECT_FALSE(offered(f, {f.driven}).has_value());
    f.options.aim.reset();
    EXPECT_FALSE(offered(f, {f.turret_a}).has_value());
}

TEST(TurretCommands, AUnitUnderTheCrosshairIsNotBareGround) {
    Field f = make_field();
    // The enemy tank, dead ahead of a level camera.
    const sim::Unit* tank = f.sim.find(f.enemy);
    f.options.camera
        = Camera{static_cast<float>(tank->x.raw) / 65536.0F * kMetresPerTile,
                 ground_metres(kSeed, tank->x, tank->y) + 0.9F,
                 (static_cast<float>(tank->y.raw) / 65536.0F - 20.0F) * kMetresPerTile, 0.0F, 0.0F};
    ASSERT_TRUE(pick(f.sim, f.options, 640.0F, 360.0F).has_unit());
    EXPECT_FALSE(offered(f, {f.turret_a}).has_value());
}

TEST(TurretCommands, SpaceSendsEveryTurretToTheSpotAndEndsTheCommand) {
    Field f = make_field();
    ASSERT_EQ(f.row.move_into(f.sim, std::vector<UnitId>{f.turret_a, f.turret_b}, 4),
              RowMove::Moved);
    look_at_ground(f);
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 3), f.sim, f.options);
    ASSERT_EQ(player.selection().size(), 2U);
    const Pick picked = pick(f.sim, f.options, 640.0F, 360.0F);

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U);
    EXPECT_LT(orders.at(0).unit, orders.at(1).unit) << "ascending id, as every verb emits";
    for (const sim::Order& order : orders) {
        ASSERT_EQ(order.kind(), sim::OrderKind::Deploy);
        EXPECT_EQ(std::get<sim::orders::Deploy>(order.payload).tile_x, picked.tile_x);
        EXPECT_EQ(std::get<sim::orders::Deploy>(order.payload).tile_y, picked.tile_y);
    }
    EXPECT_TRUE(player.selection().empty());
}

TEST(TurretCommands, TheMenuOffersSpaceDeployOverGroundAndTheAttackWinsOverIt) {
    Field f = make_field();
    look_at_ground(f);
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1 + 3), f.sim, f.options, orders); // F4: turret A
    ASSERT_EQ(session.selection().size(), 1U);
    std::vector<MenuRow> rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE");
    EXPECT_EQ(rows.front().verb, "Deploy");

    // Aimed at the hostile tank instead, the state row is the attack: it owns SPACE first.
    const sim::Unit* tank = f.sim.find(f.enemy);
    f.options.camera
        = Camera{static_cast<float>(tank->x.raw) / 65536.0F * kMetresPerTile,
                 ground_metres(kSeed, tank->x, tank->y) + 0.9F,
                 (static_cast<float>(tank->y.raw) / 65536.0F - 8.0F) * kMetresPerTile, 0.0F, 0.0F};
    rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().verb, "Attack");
}

TEST(TurretCommands, FollowMeSendsTheTurretsAfterTheDrivenCraft) {
    Field f = make_field();
    Interaction player = select(f, f.turret_a);
    ASSERT_EQ(player.selection().size(), 1U);
    player.handle(key_frame(input::kKey1), f.sim, f.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, f.turret_a);
    ASSERT_EQ(orders.front().kind(), sim::OrderKind::Follow);
    EXPECT_EQ(std::get<sim::orders::Follow>(orders.front().payload).target, f.driven);
    EXPECT_TRUE(player.selection().empty());
}

TEST(TurretCommands, TheUsualVehicleCommandsWorkOnATurretToo) {
    Field f = make_field();
    Interaction hold = select(f, f.turret_a);
    hold.handle(key_frame(input::kKey1 + 3), f.sim, f.options); // 4 Hold
    const std::vector<sim::Order> posted = hold.take_orders();
    ASSERT_EQ(posted.size(), 1U);
    EXPECT_EQ(posted.front().kind(), sim::OrderKind::GuardAt);

    Interaction recycle = select(f, f.turret_b);
    recycle.handle(key_frame(input::kKey0), f.sim, f.options); // 0 Recycle: no recycler here
    EXPECT_TRUE(recycle.take_orders().empty());
    EXPECT_EQ(recycle.message(), "There is no recycler to recycle at.");
}

TEST(TurretCommands, DefendSendsEachTurretAtTheNearestHostileItCanSee) {
    Field f = make_field();
    Interaction player = select(f, f.turret_a);
    player.handle(key_frame(input::kKey1 + 5), f.sim, f.options); // 6 Defend
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, f.turret_a);
    ASSERT_EQ(orders.front().kind(), sim::OrderKind::Defend);
    EXPECT_EQ(std::get<sim::orders::Defend>(orders.front().payload).target, f.enemy);
    EXPECT_TRUE(player.selection().empty()) << "the command is done";
    for (const sim::Order& order : orders) {
        f.sim.enqueue(order);
    }
    EXPECT_EQ(f.sim.command_log().size(), 1U) << "and the sim takes it";
}

TEST(TurretCommands, DefendWithNoEnemyInSightIsRefusedAloud) {
    Field f = make_field();
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.spawns
        = {{UnitKind::Tank, Side::Player, 36, 44}, {UnitKind::Turret, Side::Player, 34, 50}};
    f.sim = sim::Sim::create(scenario);
    f.driven = f.sim.units()[0].id;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    f.row.update(f.sim, Side::Player, 0);
    Interaction player = select(f, f.sim.units()[1].id);
    ASSERT_EQ(player.selection().size(), 1U);
    player.handle(key_frame(input::kKey1 + 5), f.sim, f.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "no enemies within radar range.");
    EXPECT_EQ(player.selection().size(), 1U) << "a refusal keeps the selection";
}

TEST(TurretCommands, UndeployTakesDownADeployedTurretAndRefusesAPlainOne) {
    Field f = make_field();
    Interaction plain = select(f, f.turret_a);
    plain.handle(key_frame(input::kKey1 + 6), f.sim, f.options); // 7 Undeploy: nothing is deployed
    EXPECT_TRUE(plain.take_orders().empty());

    // Deploy turret B in the sim, then press 7 on it.
    f.sim.enqueue(
        sim::Order{f.turret_b, sim::orders::Deploy{f.sim.find(f.turret_b)->x.floor_int(),
                                                   f.sim.find(f.turret_b)->y.floor_int()}});
    for (std::uint32_t i = 0; i < sim::kTurretDeployTicks + 5; ++i) {
        f.sim.step();
    }
    ASSERT_EQ(f.sim.find(f.turret_b)->kind, UnitKind::DeployedTurret);
    f.row.update(f.sim, Side::Player, 0);
    Interaction standing = select(f, f.turret_b);
    ASSERT_EQ(standing.selection().size(), 1U) << "a deployed turret is still on its F key";
    standing.handle(key_frame(input::kKey1 + 6), f.sim, f.options);
    const std::vector<sim::Order> orders = standing.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, f.turret_b);
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::Undeploy);
    EXPECT_TRUE(standing.selection().empty());
}

TEST(TurretCommands, TheDeployedTurretKeepsItsIdAndItsFKeyThereAndBack) {
    // The kind changes BOTH WAYS for the first time, so the row must follow it: the same unit stays
    // on the same F key through plain -> deployed -> plain -> deployed.
    Field f = make_field();
    const auto slot_of = [&f](UnitId id) -> int {
        for (std::size_t slot = 0; slot < kRowSlots; ++slot) {
            const auto& units = f.row.slots.at(slot).units;
            if (std::find(units.begin(), units.end(), id) != units.end()) {
                return static_cast<int>(slot);
            }
        }
        return -1;
    };
    const int slot = slot_of(f.turret_a);
    ASSERT_GE(slot, static_cast<int>(kRowFirstGroup));
    const auto step = [&f](std::uint32_t ticks) {
        for (std::uint32_t i = 0; i < ticks; ++i) {
            f.sim.step();
        }
        f.row.update(f.sim, Side::Player, 0);
    };
    const int tx = f.sim.find(f.turret_a)->x.floor_int();
    const int ty = f.sim.find(f.turret_a)->y.floor_int();
    f.sim.enqueue(sim::Order{f.turret_a, sim::orders::Deploy{tx, ty}});
    step(sim::kTurretDeployTicks + 5);
    ASSERT_EQ(f.sim.find(f.turret_a)->kind, UnitKind::DeployedTurret);
    EXPECT_EQ(slot_of(f.turret_a), slot) << "deployed: same key";
    ASSERT_TRUE(read_slot(f.sim, f.row.slots.at(static_cast<std::size_t>(slot))).has_value());
    EXPECT_EQ(read_slot(f.sim, f.row.slots.at(static_cast<std::size_t>(slot)))
                  .value_or(SlotReading{})
                  .kind,
              UnitKind::DeployedTurret);

    f.sim.enqueue(sim::Order{f.turret_a, sim::orders::Undeploy{}});
    step(sim::kTurretUndeployTicks + 2);
    ASSERT_EQ(f.sim.find(f.turret_a)->kind, UnitKind::Turret);
    EXPECT_EQ(slot_of(f.turret_a), slot) << "and back: still the same key";
    EXPECT_EQ(read_slot(f.sim, f.row.slots.at(static_cast<std::size_t>(slot)))
                  .value_or(SlotReading{})
                  .kind,
              UnitKind::Turret);
    EXPECT_EQ(f.sim.find(f.turret_a)->id, f.turret_a);

    f.sim.enqueue(sim::Order{f.turret_a, sim::orders::Deploy{tx, ty}});
    step(sim::kTurretDeployTicks + 5);
    EXPECT_EQ(f.sim.find(f.turret_a)->kind, UnitKind::DeployedTurret);
    EXPECT_EQ(slot_of(f.turret_a), slot) << "round the loop and the key has not moved";
    // ...and no other key picked it up on the way.
    int holders = 0;
    for (const RowSlot& each : f.row.slots) {
        holders += static_cast<int>(std::count(each.units.begin(), each.units.end(), f.turret_a));
    }
    EXPECT_EQ(holders, 1);
}

TEST(TurretCommands, ADeployedTurretCanBeSelectedAndOrderedThroughItsKey) {
    Field f = make_field();
    f.sim.enqueue(
        sim::Order{f.turret_a, sim::orders::Deploy{f.sim.find(f.turret_a)->x.floor_int(),
                                                   f.sim.find(f.turret_a)->y.floor_int()}});
    for (std::uint32_t i = 0; i < sim::kTurretDeployTicks + 5; ++i) {
        f.sim.step();
    }
    f.row.update(f.sim, Side::Player, 0);
    Interaction player = select(f, f.turret_a);
    ASSERT_EQ(player.selection().size(), 1U);
    EXPECT_EQ(menu_subject_of(f.sim, player.selection().units()), MenuSubject::Turret);
    player.handle(key_frame(input::kKey1), f.sim,
                  f.options); // Follow Me: it undeploys, then follows
    ASSERT_EQ(player.take_orders().size(), 1U);
}

// ======================================================================= the verbs, in cmd::

TEST(TurretCommands, TheCmdVerbsEmitOneOrderPerUnitInAscendingIdOrder) {
    const std::vector<UnitId> roster{UnitId{7}, UnitId{3}};
    const std::vector<sim::Order> deploy = cmd::deploy(roster, 12, 34);
    ASSERT_EQ(deploy.size(), 2U);
    EXPECT_EQ(deploy.at(0).unit.value, 3U);
    EXPECT_EQ(std::get<sim::orders::Deploy>(deploy.at(0).payload).tile_x, 12);
    EXPECT_EQ(std::get<sim::orders::Deploy>(deploy.at(0).payload).tile_y, 34);
    const std::vector<sim::Order> defend = cmd::defend(roster, UnitId{9});
    ASSERT_EQ(defend.size(), 2U);
    EXPECT_EQ(std::get<sim::orders::Defend>(defend.at(1).payload).target.value, 9U);
    EXPECT_EQ(cmd::undeploy(roster).size(), 2U);
}

TEST(TurretCommands, TheCmdVerbsDropAnImpossibleOrderRatherThanRepairIt) {
    const std::vector<UnitId> roster{UnitId{3}};
    EXPECT_TRUE(cmd::deploy(roster, -1, 5).empty());
    EXPECT_TRUE(cmd::deploy(roster, 5, sim::kMapTiles).empty());
    EXPECT_TRUE(cmd::defend(roster, UnitId{}).empty());
}

} // namespace
} // namespace protekt::game
