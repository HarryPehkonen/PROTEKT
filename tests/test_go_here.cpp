// SPACE GO HERE: any vehicle, or any group under a function key, pointed at bare ground, is offered
// `SPACE Go Here` on the left menu, and SPACE sends all of it there (Harri, 2026-11-03: "I should
// also be able to select any vehicle or group of vehicles under a function key, point at the
// ground, and say SPACE Go Here"). Before this the verb did not exist: SPACE on bare ground did
// nothing for anything but turrets (which deploy). The offer, the order and the menu row all come
// from ONE function, `go_here_offer`, so the prompt and the key cannot name two places.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/go_here_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

struct Field {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;
    UnitId tank_a;
    UnitId tank_b;
    UnitId tank_c;
    UnitId bike;
    UnitId factory;
    UnitId enemy;
};

/// The camera is raised well clear of the terrain and pitched down, so the crosshair's ray meets
/// bare ground ahead with nothing standing there. The player's tanks share F1 (the start rule seats
/// one key per kind), the mortar bike has F2, and the enemy is out of every cone.
[[nodiscard]] Field make_field() {
    Field f{};
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Player, 30, 30},
                                            {UnitKind::Tank, Side::Player, 31, 30},
                                            {UnitKind::Tank, Side::Player, 32, 30},
                                            {UnitKind::MortarBike, Side::Player, 33, 30},
                                            {UnitKind::Factory, Side::Player, 20, 20},
                                            {UnitKind::Tank, Side::Enemy, 50, 62}}});
    const auto& units = f.sim.units();
    f.driven = units[0].id;
    f.tank_a = units[1].id;
    f.tank_b = units[2].id;
    f.tank_c = units[3].id;
    f.bike = units[4].id;
    f.factory = units[5].id;
    f.enemy = units[6].id;
    f.options.seed = kSeed;
    f.options.extent = kExtent;
    f.options.driven_unit = f.driven;
    f.options.viewer_unit = f.driven;
    f.options.camera
        = Camera{80.0F, ground_metres(kSeed, 40.0F, 60.0F) + 30.0F, 80.0F, 0.0F, -0.6F};
    f.options.aim = Aim{640.0F, 360.0F};
    f.options.row = &f.row;
    f.row.seed_from_start(f.sim, Side::Player, 0, f.driven);
    return f;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] std::optional<GoHereOffer> offered(const Field& f, std::vector<UnitId> selected) {
    return go_here_offer(f.sim, f.options, selected, f.driven);
}

} // namespace

TEST(GoHere, AGroupUnderAFunctionKeyIsOfferedTheGroundTheCrosshairIsOn) {
    Field f = make_field();
    const Pick ground = pick(f.sim, f.options, 640.0F, 360.0F);
    ASSERT_TRUE(ground.has_tile());
    ASSERT_FALSE(ground.has_unit()) << "the rig must point at bare ground";
    const std::optional<GoHereOffer> offer = offered(f, {f.tank_a, f.tank_b, f.tank_c});
    ASSERT_TRUE(offer.has_value());
    EXPECT_EQ(offer.value_or(GoHereOffer{}).tile_x, ground.tile_x);
    EXPECT_EQ(offer.value_or(GoHereOffer{}).tile_y, ground.tile_y);
}

TEST(GoHere, ALoneVehicleIsOfferedItToo) {
    Field f = make_field();
    EXPECT_TRUE(offered(f, {f.bike}).has_value());
    EXPECT_TRUE(offered(f, {f.tank_a}).has_value());
}

TEST(GoHere, NothingIsOfferedWithNothingSelectedOrNoCrosshairOrOnlyTheDrivenTank) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {}).has_value()) << "nothing selected";
    EXPECT_FALSE(offered(f, {f.driven}).has_value()) << "the tank you sit in takes no order";
    f.options.aim.reset();
    EXPECT_FALSE(offered(f, {f.tank_a}).has_value()) << "no crosshair, nothing to point with";
}

TEST(GoHere, ABuildingCannotGoAnywhereSoItIsNeverOfferedIt) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {f.factory}).has_value());
}

TEST(GoHere, AUnitUnderTheCrosshairIsNotGround) {
    // Look straight at tank_a from short of it: a unit under the mark is a follow or an attack.
    Field f = make_field();
    const sim::Unit* tank = f.sim.find(f.tank_a);
    f.options.camera = Camera{
        static_cast<float>(tank->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
        ground_metres(kSeed, tank->x, tank->y) + 0.9F,
        (static_cast<float>(tank->y.raw) / static_cast<float>(sim::Fixed::kOne) - 10.0F)
            * kMetresPerTile,
        0.0F, 0.0F};
    ASSERT_TRUE(pick(f.sim, f.options, 640.0F, 360.0F).has_unit());
    EXPECT_FALSE(offered(f, {f.bike}).has_value());
}

TEST(GoHere, TheMenuReadsGoHereOnTheStateRowForAGroup) {
    Field f = make_field();
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1), f.sim, f.options, orders);
    ASSERT_EQ(session.selection().size(), 3U) << "F1 holds the three tanks";
    const std::vector<MenuRow> rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE");
    EXPECT_EQ(rows.front().verb, "Go Here");
}

TEST(GoHere, TheRowIsNotThereWhenThereIsNothingToPointAt) {
    Field f = make_field();
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1), f.sim, f.options, orders);
    f.options.aim.reset();
    const std::vector<MenuRow> rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_NE(rows.front().verb, "Go Here");
}

TEST(GoHere, SpaceSendsTheWholeGroupToTheOfferedTileAndClosesTheMenu) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1), f.sim, f.options);
    ASSERT_EQ(player.selection().size(), 3U);
    const std::optional<GoHereOffer> offer = offered(f, {f.tank_a, f.tank_b, f.tank_c});
    ASSERT_TRUE(offer.has_value());

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 3U) << "one MoveTo per tank in the group";
    for (const sim::Order& order : orders) {
        const auto* move = std::get_if<sim::orders::MoveTo>(&order.payload);
        ASSERT_NE(move, nullptr);
        EXPECT_EQ(move->x.floor_int(), offer.value_or(GoHereOffer{}).tile_x);
        EXPECT_EQ(move->y.floor_int(), offer.value_or(GoHereOffer{}).tile_y);
    }
    EXPECT_TRUE(player.selection().empty()) << "the command is done: the selection goes";
}

TEST(GoHere, SpaceSendsALoneVehicleThere) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 1), f.sim, f.options); // F2: the mortar bike
    ASSERT_EQ(player.selection().size(), 1U);
    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, f.bike);
    EXPECT_TRUE(std::holds_alternative<sim::orders::MoveTo>(orders.front().payload));
}

TEST(GoHere, SpaceWithNothingSelectedStillOnlySelectsAndNeverMoves) {
    Field f = make_field();
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(GoHere, AnEnemyUnderTheCrosshairStillOwnsSpaceAsTheAttack) {
    // The order of precedence is unchanged: attack first. Point straight at the enemy tank.
    Field f = make_field();
    const sim::Unit* enemy = f.sim.find(f.enemy);
    f.options.camera = Camera{
        static_cast<float>(enemy->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
        ground_metres(kSeed, enemy->x, enemy->y) + 0.9F,
        (static_cast<float>(enemy->y.raw) / static_cast<float>(sim::Fixed::kOne) - 20.0F)
            * kMetresPerTile,
        0.0F, 0.0F};
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1), f.sim, f.options);
    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_FALSE(orders.empty());
    EXPECT_FALSE(std::holds_alternative<sim::orders::MoveTo>(orders.front().payload))
        << "an attack, not a move";
}

} // namespace protekt::game
