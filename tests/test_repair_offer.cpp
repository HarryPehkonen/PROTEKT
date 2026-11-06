// SPACE REPAIR (D1b, repair by TARGETING). Harri, 2026-11-05: "maybe the same logic that's used to
// attack an enemy can be used to repair a friendly. You know, targetting." With a SERVICE TRUCK
// selected, pointing -- approximately -- at a DAMAGED friendly offers `SPACE Repair` on the left
// menu, SPACE sends the truck, and the scene draws a GREEN line from the menu to the patient and a
// green dot on it: the mirror of the attack's red ("the line says whose the thing is").
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/cmd/command.hpp"
#include "protekt/game/attack_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/repair_aim.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

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
constexpr std::uint16_t kHurt = 10;

struct Field {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;  ///< F1: the tank the player sits in
    UnitId tank;    ///< F2: an armed tank, selectable
    UnitId truck;   ///< F3: the service truck
    UnitId patient; ///< a hurt friendly tank 40 m dead ahead of the camera
    UnitId enemy;
};

/// The attack offer's rig, with the enemy tank replaced by a hurt friendly one: the camera stands
/// 20 tiles short of the patient's tile at the height of its middle, looking straight at it (yaw 0
/// is +Z), so a yaw of `d` puts it exactly `d` off the crosshair with a level ray.
[[nodiscard]] Field make_field(std::uint16_t patient_hp = kHurt, Side patient_side = Side::Player) {
    Field f{};
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Player, 30, 30},
                                            {UnitKind::ServiceTruck, Side::Player, 31, 30},
                                            {UnitKind::Tank, patient_side, 40, 60, patient_hp},
                                            {UnitKind::Factory, Side::Enemy, 150, 150}}});
    const auto ids = f.sim.units();
    f.driven = ids[0].id;
    f.tank = ids[1].id;
    f.truck = ids[2].id;
    f.patient = ids[3].id;
    f.enemy = ids[4].id;
    const sim::Unit* target = f.sim.find(f.patient);
    f.options.seed = kSeed;
    f.options.extent = kExtent;
    f.options.camera
        = Camera{80.0F, ground_metres(kSeed, target->x, target->y) + 0.9F, 80.0F, 0.0F, 0.0F};
    f.options.aim = Aim{640.0F, 360.0F};
    f.row.update(f.sim, Side::Player, 0);
    return f;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] UnitId offered(const Field& f, std::vector<UnitId> selected) {
    return repair_target(f.sim, f.options, selected, f.driven);
}

} // namespace

// ------------------------------------------------------------------------------ the offer

TEST(RepairOffer, ASelectedTruckPointedAtADamagedFriendIsOfferedIt) {
    Field f = make_field();
    ASSERT_LT(f.sim.find(f.patient)->hp, sim::spec_of(UnitKind::Tank).hp);
    EXPECT_EQ(offered(f, {f.truck}), f.patient);
}

TEST(RepairOffer, ATruckPointedAtAnUndamagedFriendIsOfferedNothing) {
    Field f = make_field(sim::spec_of(UnitKind::Tank).hp);
    ASSERT_EQ(f.sim.find(f.patient)->hp, sim::spec_of(UnitKind::Tank).hp);
    EXPECT_FALSE(offered(f, {f.truck}).valid()) << "nothing to mend, so no 'SPACE Repair'";
}

TEST(RepairOffer, WithNothingSelectedThereIsNoOffer) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {}).valid()) << "a lone unit gets the plain Select, not a repair";
}

TEST(RepairOffer, ASelectionWithoutATruckIsOfferedNothing) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {f.tank}).valid()) << "only a truck can mend";
}

TEST(RepairOffer, AMixedSelectionIsOfferedWhenItHoldsATruck) {
    Field f = make_field();
    EXPECT_EQ(offered(f, {f.tank, f.truck}), f.patient);
}

TEST(RepairOffer, AnEnemyUnderTheCrosshairIsNotAPatient) {
    Field f = make_field(kHurt, Side::Enemy);
    EXPECT_FALSE(offered(f, {f.truck}).valid()) << "that is the attack's, and a truck is unarmed";
}

TEST(RepairOffer, TheDrivenTankIsNeverThePatient) {
    // The eye sits on the driven hull's barrel, so it is the thing being looked out of.
    Field f = make_field();
    f.options.driven_unit = f.patient;
    EXPECT_FALSE(repair_target(f.sim, f.options, std::vector<UnitId>{f.truck}, f.patient).valid());
}

TEST(RepairOffer, TheTruckItselfIsNotItsOwnPatient) {
    Field f = make_field();
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::ServiceTruck, Side::Player, 40, 60, kHurt},
                                            {UnitKind::Factory, Side::Enemy, 150, 150}}});
    const UnitId truck = f.sim.units()[1].id;
    ASSERT_LT(f.sim.find(truck)->hp, sim::spec_of(UnitKind::ServiceTruck).hp);
    EXPECT_FALSE(repair_target(f.sim, f.options, std::vector<UnitId>{truck}, f.driven).valid())
        << "an order naming itself is refused by the sim, so it is never offered";
}

TEST(RepairOffer, AFriendJustInsideTheAttackWindowIsOfferedAndJustOutsideIsNot) {
    // The ONE aim: the repair is offered inside exactly the cone the attack is, because both ask
    // the same scan.
    Field f = make_field();
    f.options.camera.yaw = (kAttackAimToleranceDegrees - 0.5F) * kRadiansPerDegree;
    ASSERT_FALSE(pick(f.sim, f.options, 640.0F, 360.0F).has_unit())
        << "the crosshair is off the tank's box, so this is the angle rule and not the picker";
    EXPECT_EQ(offered(f, {f.truck}), f.patient) << "inside the window";
    f.options.camera.yaw = (kAttackAimToleranceDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(offered(f, {f.truck}).valid()) << "outside the window";
}

TEST(RepairOffer, WithNoCrosshairThereIsNothingToPointWith) {
    Field f = make_field();
    f.options.aim.reset();
    EXPECT_FALSE(offered(f, {f.truck}).valid());
}

TEST(RepairOffer, AWholeFriendInFrontOfAHurtOneHidesIt) {
    // The attack's rule, mirrored: what is exactly under the crosshair is the answer, and a thing
    // that is not a patient is no answer rather than a licence to look behind it.
    Field f = make_field();
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::ServiceTruck, Side::Player, 31, 30},
                                            {UnitKind::Tank, Side::Player, 40, 50},
                                            {UnitKind::Tank, Side::Player, 40, 60, kHurt}}});
    ASSERT_TRUE(pick(f.sim, f.options, 640.0F, 360.0F).has_unit());
    EXPECT_FALSE(repair_target(f.sim, f.options, std::vector<UnitId>{f.sim.units()[1].id},
                               f.sim.units()[0].id)
                     .valid());
}

// ------------------------------------------------------------------------------ SPACE

TEST(RepairKeys, SpaceSendsTheTruckWithTheCommandLayersRepairOrder) {
    Field f = make_field();
    f.options.row = &f.row;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 2), f.sim, f.options); // F3: the truck
    ASSERT_EQ(player.selection().size(), 1U);
    ASSERT_EQ(player.selection().units().front(), f.truck);

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    const std::vector<sim::Order> expected = cmd::repair(std::vector<UnitId>{f.truck}, f.patient);
    ASSERT_EQ(expected.size(), 1U);
    EXPECT_EQ(orders.front().unit, f.truck);
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::Repair);
    EXPECT_EQ(std::get<sim::orders::Repair>(orders.front().payload).target, f.patient);
    EXPECT_EQ(player.message(), "SPACE Repair");
    EXPECT_TRUE(player.selection().empty()) << "a finished command closes the menu";
}

TEST(RepairKeys, ASpaceOnAWholeFriendSendsNothing) {
    Field f = make_field(sim::spec_of(UnitKind::Tank).hp);
    f.options.row = &f.row;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 2), f.sim, f.options);
    player.handle(key_frame(input::kKeySpace), f.sim, f.options);
    for (const sim::Order& order : player.take_orders()) {
        EXPECT_NE(order.kind(), sim::OrderKind::Repair);
    }
}

// ------------------------------------------------------------------------------ the menu

TEST(RepairMenu, TheStateRowReadsSpaceRepairWhenARepairIsOffered) {
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.repair = true;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE");
    EXPECT_EQ(rows.front().verb, "Repair") << "exactly 'SPACE Repair'";
    EXPECT_TRUE(rows.front().enabled);
}

TEST(RepairMenu, WithoutAnOfferTheRowDoesNotSayRepair) {
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.target = MenuTarget::Selectable;
    EXPECT_NE(menu_rows(in).front().verb, "Repair");
}

TEST(RepairMenu, AnAttackOrAnArmedCommandKeepsSpaceForItself) {
    MenuInput in;
    in.subject = MenuSubject::Recycler;
    in.repair = true;
    in.attack = true;
    EXPECT_EQ(menu_rows(in).front().verb, "Attack");
    in.attack = false;
    in.armed = ArmedCommand{MenuCommand::Deploy};
    EXPECT_EQ(menu_rows(in).front().verb, "Deploy");
}

TEST(RepairMenu, TheAppOffersWhatTheSessionWouldRepairAndPutsItOnTheRow) {
    Field f = make_field();
    f.options.row = &f.row;
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1 + 2), f.sim, f.options, orders);
    ASSERT_EQ(session.selection().size(), 1U);

    EXPECT_EQ(app::repair_offer(f.sim, session, f.options), f.patient);
    const std::vector<MenuRow> rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().verb, "Repair");

    f.options.camera.yaw = (kAttackAimToleranceDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(app::repair_offer(f.sim, session, f.options).valid())
        << "gone the moment they turn";
    EXPECT_NE(app::menu_for(f.sim, session, f.options, false).front().verb, "Repair");
}

// ------------------------------------------------------------------------------ the drawing

namespace {

constexpr Extent kFrame{800, 450};

struct Watch {
    sim::Sim sim;
    SceneOptions options;
    UnitId patient;
};

[[nodiscard]] Watch watch_patient() {
    Watch w{};
    w.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 50, 50},
                                            {UnitKind::ServiceTruck, Side::Player, 58, 66},
                                            {UnitKind::Tank, Side::Player, 50, 62, kHurt}}});
    w.patient = w.sim.units().back().id;
    w.options.seed = kSeed;
    w.options.radius_tiles = 20;
    w.options.extent = kFrame;
    w.options.viewer_unit = w.sim.units().front().id;
    w.options.camera = Camera{100.0F, 14.0F, 76.0F, 0.0F, -0.4F};
    w.options.show_ledger = false;
    return w;
}

[[nodiscard]] std::vector<MenuRow> repair_rows() {
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.repair = true;
    return menu_rows(in);
}

[[nodiscard]] MenuLayout layout_for(const std::vector<MenuRow>& rows, const Extent& extent) {
    const float px = static_cast<float>(extent.height) / 720.0F;
    return menu_layout(rows, 6.0F * px, protekt::game::kMenuTopPixels * px, 2.0F * px);
}

[[nodiscard]] std::vector<std::size_t> green_vertices(const Scene& scene) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        if (scene.vertices.at(i).colour == kRepairLineInk) {
            found.push_back(i);
        }
    }
    return found;
}

[[nodiscard]] std::array<float, 2> pixel_of(const SceneOptions& options, const SceneVertex& v) {
    const Projected p
        = project(options.camera, options.extent, v.position[0], v.position[1], v.position[2]);
    return {p.x, p.y};
}

} // namespace

TEST(RepairLine, TheInkIsGreenAndNotTheAttacksRedNorTheDeploysWhite) {
    EXPECT_GT(kRepairLineInk.at(1), kRepairLineInk.at(0));
    EXPECT_GT(kRepairLineInk.at(1), kRepairLineInk.at(2));
    EXPECT_NE(kRepairLineInk, kAttackLineInk);
    EXPECT_NE(kRepairLineInk, kDeployLineInk);
}

TEST(RepairLine, AGreenLineRunsFromTheSpaceRepairRowToTheExactPositionOfThePatient) {
    Watch w = watch_patient();
    const std::vector<MenuRow> rows = repair_rows();
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.repair_target = w.patient;
    const Scene scene = build_scene(w.sim, w.options);

    const std::vector<std::size_t> at = green_vertices(scene);
    ASSERT_GE(at.size(), 6U) << "the line's quad, then the dot";
    // emit_quad's winding: a b c, a c d. a and d are the start edge, b and c the end edge.
    const std::array<float, 2> a = pixel_of(w.options, scene.vertices.at(at.at(0)));
    const std::array<float, 2> b = pixel_of(w.options, scene.vertices.at(at.at(1)));
    const std::array<float, 2> c = pixel_of(w.options, scene.vertices.at(at.at(2)));
    const std::array<float, 2> d = pixel_of(w.options, scene.vertices.at(at.at(5)));
    const std::array<float, 2> start{(a.at(0) + d.at(0)) * 0.5F, (a.at(1) + d.at(1)) * 0.5F};
    const std::array<float, 2> end{(b.at(0) + c.at(0)) * 0.5F, (b.at(1) + c.at(1)) * 0.5F};

    const std::array<float, 2> origin = attack_line_origin(layout_for(rows, w.options.extent));
    EXPECT_NEAR(start.at(0), origin.at(0), 0.05F) << "the same place the attack line leaves";
    EXPECT_NEAR(start.at(1), origin.at(1), 0.05F);
    const Projected target = attack_point(w.options, *w.sim.find(w.patient));
    EXPECT_NEAR(end.at(0), target.x, 0.05F) << "and it ends ON the patient";
    EXPECT_NEAR(end.at(1), target.y, 0.05F);
}

TEST(RepairLine, AGreenDotSitsOnThePatient) {
    Watch w = watch_patient();
    const std::vector<MenuRow> rows = repair_rows();
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.repair_target = w.patient;
    const Scene scene = build_scene(w.sim, w.options);

    const std::vector<std::size_t> at = green_vertices(scene);
    ASSERT_GT(at.size(), 6U + 9U) << "one quad, then a fan of at least three triangles";
    ASSERT_EQ((at.size() - 6U) % 3U, 0U) << "the dot is whole triangles";
    const Projected target = attack_point(w.options, *w.sim.find(w.patient));
    // Each dot triangle's first vertex is its centre.
    const std::array<float, 2> centre = pixel_of(w.options, scene.vertices.at(at.at(6)));
    EXPECT_NEAR(centre.at(0), target.x, 0.5F);
    EXPECT_NEAR(centre.at(1), target.y, 0.5F);
}

TEST(RepairLine, ItIsGoneTheMomentThePatientIsNotOffered) {
    Watch w = watch_patient();
    const std::vector<MenuRow> rows = repair_rows();
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.repair_target = UnitId{};
    EXPECT_TRUE(green_vertices(build_scene(w.sim, w.options)).empty());
}

TEST(RepairLine, AMenuThatDoesNotSayRepairDrawsNoLine) {
    Watch w = watch_patient();
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    const std::vector<MenuRow> rows = menu_rows(in); // SPACE with no verb: no offer
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.repair_target = w.patient;
    EXPECT_TRUE(green_vertices(build_scene(w.sim, w.options)).empty());
}

TEST(RepairLine, WithNoPatientTheFrameIsByteIdenticalToOneThatNeverHeardOfIt) {
    Watch w = watch_patient();
    const std::vector<MenuRow> rows = repair_rows();
    w.options.show_menu = true;
    w.options.menu = &rows;
    const Scene bare = build_scene(w.sim, w.options);
    w.options.repair_target = UnitId{};
    const Scene same = build_scene(w.sim, w.options);
    ASSERT_EQ(bare.vertices.size(), same.vertices.size());
    for (std::size_t i = 0; i < bare.vertices.size(); ++i) {
        EXPECT_EQ(bare.vertices.at(i).position, same.vertices.at(i).position);
    }
}

} // namespace protekt::game
