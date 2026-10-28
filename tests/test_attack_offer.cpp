// SPACE ATTACK: with something armed selected, pointing -- approximately -- at an enemy asset
// offers `SPACE Attack` on the left menu, SPACE gives the order, and the scene draws a red line
// from the menu to the asset, a label of its kind above it and a health bar under the label. `T` no
// longer gives the pointed-at attack.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <variant>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/cmd/command.hpp"
#include "protekt/game/attack_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
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

struct Field {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;    ///< F1: the tank the player sits in, kept out of the crosshair's way
    UnitId armed;     ///< F2: a tank, the thing that is selected
    UnitId scavenger; ///< F3: unarmed
    UnitId turret;    ///< F4: armed, and not a tank
    UnitId enemy;     ///< a hostile tank 40 m dead ahead of the camera
    UnitId enemy_factory;
};

/// The camera stands 20 tiles short of the enemy tank at the height of the tank's middle, looking
/// straight at it (yaw 0 is +Z), so a yaw of `d` puts the tank exactly `d` off the crosshair and
/// the crosshair's ray is horizontal: the angle is the yaw, with no pitch to blur it.
[[nodiscard]] Field make_field() {
    Field f{};
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Player, 30, 30},
                                            {UnitKind::Scavenger, Side::Player, 31, 30},
                                            {UnitKind::Turret, Side::Player, 32, 30},
                                            {UnitKind::Tank, Side::Enemy, 40, 60},
                                            {UnitKind::Factory, Side::Enemy, 50, 62}}});
    const auto ids = f.sim.units();
    f.driven = ids[0].id;
    f.armed = ids[1].id;
    f.scavenger = ids[2].id;
    f.turret = ids[3].id;
    f.enemy = ids[4].id;
    f.enemy_factory = ids[5].id;
    const sim::Unit* tank = f.sim.find(f.enemy);
    f.options.seed = kSeed;
    f.options.extent = kExtent;
    f.options.camera
        = Camera{80.0F, ground_metres(kSeed, tank->x, tank->y) + 0.9F, 80.0F, 0.0F, 0.0F};
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
    return attack_target(f.sim, f.options, selected, f.driven);
}

} // namespace

// ------------------------------------------------------------------------------ the offer (a)

TEST(AttackOffer, AToleranceOfAFewDegreesIsNamedOnceHere) {
    EXPECT_GE(kAttackAimToleranceDegrees, 1.0F) << "narrower than a tank's width at range";
    EXPECT_LE(kAttackAimToleranceDegrees, 6.0F) << "wider and the offer no longer means 'that one'";
}

TEST(AttackOffer, AnEnemyUnderTheCrosshairIsOfferedToAnArmedSelection) {
    Field f = make_field();
    EXPECT_EQ(offered(f, {f.armed}), f.enemy);
}

TEST(AttackOffer, AnEnemyJustInsideTheWindowIsOfferedAndJustOutsideIsNot) {
    Field f = make_field();
    f.options.camera.yaw = (kAttackAimToleranceDegrees - 0.5F) * kRadiansPerDegree;
    ASSERT_FALSE(pick(f.sim, f.options, 640.0F, 360.0F).has_unit())
        << "the crosshair is off the tank's box, so this is the angle rule and not the picker";
    EXPECT_EQ(offered(f, {f.armed}), f.enemy) << "inside the window";

    f.options.camera.yaw = (kAttackAimToleranceDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(offered(f, {f.armed}).valid()) << "outside the window";
}

TEST(AttackOffer, TheWindowIsTheSameOnTheOtherSide) {
    Field f = make_field();
    f.options.camera.yaw = -(kAttackAimToleranceDegrees - 0.5F) * kRadiansPerDegree;
    EXPECT_EQ(offered(f, {f.armed}), f.enemy);
    f.options.camera.yaw = -(kAttackAimToleranceDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(offered(f, {f.armed}).valid());
}

TEST(AttackOffer, AFriendlyUnderTheCrosshairIsNeverOffered) {
    Field f = make_field();
    // Our own tank stands exactly where the enemy did, and the enemy is gone from the ray.
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Player, 30, 30},
                                            {UnitKind::Tank, Side::Player, 40, 60}}});
    const std::vector<UnitId> own{f.sim.units()[1].id};
    f.driven = f.sim.units()[0].id;
    EXPECT_FALSE(attack_target(f.sim, f.options, own, f.driven).valid())
        << "a friendly is not a target, so no 'SPACE Attack' for it";
}

TEST(AttackOffer, AFriendlyInFrontOfAnEnemyHidesIt) {
    Field f = make_field();
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Player, 30, 30},
                                            {UnitKind::Tank, Side::Player, 40, 52},
                                            {UnitKind::Tank, Side::Enemy, 40, 60}}});
    const std::vector<UnitId> own{f.sim.units()[1].id};
    f.driven = f.sim.units()[0].id;
    EXPECT_FALSE(attack_target(f.sim, f.options, own, f.driven).valid())
        << "the crosshair is on our own tank; the enemy behind it is not what is pointed at";
}

TEST(AttackOffer, AnEnemyStructureIsAnAssetToo) {
    Field f = make_field();
    const sim::Unit* factory = f.sim.find(f.enemy_factory);
    f.options.camera.x = static_cast<float>(factory->x.raw) / 65536.0F * kMetresPerTile;
    f.options.camera.y = ground_metres(kSeed, factory->x, factory->y)
                         + (unit_box_height_metres(UnitKind::Factory) * 0.5F);
    f.options.camera.z = 80.0F;
    EXPECT_EQ(offered(f, {f.armed}), f.enemy_factory);
}

TEST(AttackOffer, AnythingWithWeaponsOffersItNotJustATank) {
    Field f = make_field();
    EXPECT_EQ(offered(f, {f.turret}), f.enemy) << "a turret has a gun";
    EXPECT_EQ(offered(f, {f.scavenger, f.armed}), f.enemy)
        << "one armed unit in the group is enough";
}

TEST(AttackOffer, AnUnarmedSelectionIsOfferedNothing) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {f.scavenger}).valid()) << "a scavenger has no weapon to attack with";
    EXPECT_FALSE(offered(f, {}).valid()) << "nothing selected";
}

TEST(AttackOffer, TheDrivenTankAloneCannotBeOrderedSoNothingIsOffered) {
    Field f = make_field();
    EXPECT_FALSE(offered(f, {f.driven}).valid());
}

TEST(AttackOffer, WithNoCrosshairThereIsNothingToPointWith) {
    Field f = make_field();
    f.options.aim.reset();
    EXPECT_FALSE(offered(f, {f.armed}).valid());
}

TEST(AttackOffer, TheNearestToTheRayWinsAndAnExactTieGoesToTheLowerId) {
    Field f = make_field();
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 36, 44},
                                            {UnitKind::Tank, Side::Player, 30, 30},
                                            {UnitKind::Tank, Side::Enemy, 42, 60},
                                            {UnitKind::Tank, Side::Enemy, 40, 60}}});
    f.driven = f.sim.units()[0].id;
    const UnitId armed = f.sim.units()[1].id;
    const UnitId off_to_the_side = f.sim.units()[2].id;
    const UnitId dead_ahead = f.sim.units()[3].id;
    ASSERT_NE(off_to_the_side, dead_ahead);
    f.options.camera.y
        = ground_metres(kSeed, f.sim.find(dead_ahead)->x, f.sim.find(dead_ahead)->y) + 0.9F;
    f.options.camera.yaw = 1.0F * kRadiansPerDegree;
    EXPECT_EQ(attack_target(f.sim, f.options, std::vector<UnitId>{armed}, f.driven), dead_ahead)
        << "the tank nearer the ray, though it has the higher id";
}

// ------------------------------------------------------------------------------ SPACE and T (b, c)

TEST(AttackKeys, SpaceAttacksTheOfferedTargetWithTheSameOrderTUsedToGive) {
    Field f = make_field();
    f.options.row = &f.row;
    f.options.camera.yaw = (kAttackAimToleranceDegrees - 0.5F) * kRadiansPerDegree;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 1), f.sim, f.options); // F2: the armed tank
    ASSERT_EQ(player.selection().size(), 1U);

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    const std::vector<sim::Order> expected = cmd::attack(std::vector<UnitId>{f.armed}, f.enemy);
    ASSERT_EQ(expected.size(), 1U);
    EXPECT_EQ(orders.front().unit, expected.front().unit);
    EXPECT_EQ(orders.front().kind(), expected.front().kind());
    EXPECT_EQ(std::get<sim::orders::Attack>(orders.front().payload).target,
              std::get<sim::orders::Attack>(expected.front().payload).target)
        << "the order is cmd::attack's, untouched: this is an input change, not a new ability";
    EXPECT_EQ(player.message(), "SPACE Attack");
}

TEST(AttackKeys, SpaceAttacksAnEnemyTankInsteadOfFollowingIt) {
    // SPACE on a tank used to FOLLOW it whoever's it was; an enemy tank under the crosshair is now
    // the attack the menu offers. (F still follows, for the player who wants to tail one.)
    Field f = make_field();
    f.options.row = &f.row;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 1), f.sim, f.options);

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::Attack);
}

TEST(AttackKeys, SpaceOutsideTheWindowAttacksNothing) {
    Field f = make_field();
    f.options.row = &f.row;
    f.options.camera.yaw = (kAttackAimToleranceDegrees + 0.5F) * kRadiansPerDegree;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 1), f.sim, f.options);

    player.handle(key_frame(input::kKeySpace), f.sim, f.options);

    EXPECT_TRUE(player.take_orders().empty()) << "what the menu does not offer, SPACE does not do";
}

TEST(AttackKeys, TNoLongerAttacksWhatTheCrosshairIsOn) {
    Field f = make_field();
    f.options.row = &f.row;
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyF1 + 1), f.sim, f.options);

    player.handle(key_frame(input::kKeyT), f.sim, f.options);

    EXPECT_TRUE(player.take_orders().empty())
        << "pressing T with an enemy dead centre gives no order: SPACE does that";
    EXPECT_TRUE(player.attack_armed()) << "T keeps its other job, the numbered attack list";
    EXPECT_NE(player.message(), "SPACE Attack");
    // ...and the list's own digit still attacks, exactly as it did.
    player.handle(key_frame(input::kKey1), f.sim, f.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::Attack);
}

// ------------------------------------------------------------------------------ the menu row (a)

TEST(AttackMenu, TheStateRowReadsSpaceAttackWhenAnAttackIsOffered) {
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.attack = true;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE");
    EXPECT_EQ(rows.front().verb, "Attack") << "exactly 'SPACE Attack'";
    EXPECT_TRUE(rows.front().enabled);
}

TEST(AttackMenu, WithoutAnOfferTheRowDoesNotSayAttack) {
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.target = MenuTarget::Hostile;
    EXPECT_NE(menu_rows(in).front().verb, "Attack") << "a hostile alone is not the offer";
    in.target = MenuTarget::Selectable;
    EXPECT_EQ(menu_rows(in).front().verb, "Select");
}

TEST(AttackMenu, AMixedSelectionStillGetsTheOfferAsASingleRow) {
    MenuInput in; // subject None: a tank and a scavenger together own no menu
    in.attack = true;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front().verb, "Attack");
    in.attack = false;
    EXPECT_TRUE(menu_rows(in).empty()) << "and no offer, no menu, as ever";
}

TEST(AttackMenu, AnArmedDeployKeepsSpaceForItself) {
    MenuInput in;
    in.subject = MenuSubject::Recycler;
    in.armed = ArmedCommand{MenuCommand::Deploy};
    in.attack = true;
    EXPECT_EQ(menu_rows(in).front().verb, "Deploy");
}

TEST(AttackMenu, TheAppOffersWhatTheSessionWouldAttackAndPutsItOnTheRow) {
    Field f = make_field();
    f.options.row = &f.row;
    game::Session session{f.driven};
    std::vector<sim::Order> orders;
    session.frame(key_frame(input::kKeyF1 + 1), f.sim, f.options, orders);
    ASSERT_EQ(session.selection().size(), 1U);

    EXPECT_EQ(app::attack_offer(f.sim, session, f.options), f.enemy);
    const std::vector<MenuRow> rows = app::menu_for(f.sim, session, f.options, false);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().verb, "Attack");

    f.options.camera.yaw = (kAttackAimToleranceDegrees + 0.5F) * kRadiansPerDegree;
    EXPECT_FALSE(app::attack_offer(f.sim, session, f.options).valid());
    EXPECT_NE(app::menu_for(f.sim, session, f.options, false).front().verb, "Attack");
}

// ------------------------------------------------------------------------------ the drawing (d, e,
// f)

namespace {

constexpr Extent kFrame{800, 450};
constexpr float kLabelInk = 0.95F;

struct Rect {
    float left{1e9F};
    float right{-1e9F};
    float top{1e9F};
    float bottom{-1e9F};
    [[nodiscard]] float width() const { return right - left; }
    [[nodiscard]] float height() const { return bottom - top; }
    [[nodiscard]] float centre_x() const { return (left + right) * 0.5F; }
};

[[nodiscard]] Rect rect_of(const SceneOptions& options, const Scene& scene, std::size_t first) {
    Rect r;
    for (std::size_t i = 0; i < 6; ++i) {
        const SceneVertex& v = scene.vertices.at(first + i);
        const Projected p
            = project(options.camera, options.extent, v.position[0], v.position[1], v.position[2]);
        r.left = std::min(r.left, p.x);
        r.right = std::max(r.right, p.x);
        r.top = std::min(r.top, p.y);
        r.bottom = std::max(r.bottom, p.y);
    }
    return r;
}

/// A scene with the camera at 800 x 450 over `sim`: the readout's fractions are of the frame.
struct Watch {
    sim::Sim sim;
    SceneOptions options;
    UnitId enemy;
};

/// An enemy tank 10 tiles in front of a high camera, and two of ours shooting at it from the side
/// (clear of the camera's line) until its hit points are at most `hp_at_most`.
[[nodiscard]] Watch watch_enemy(std::uint16_t hp_at_most) {
    Watch w{};
    w.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 50, 50},
                                            {UnitKind::Tank, Side::Player, 58, 66},
                                            {UnitKind::Tank, Side::Player, 59, 67},
                                            {UnitKind::Tank, Side::Enemy, 50, 62}}});
    w.enemy = w.sim.units().back().id;
    for (int guard = 0;
         guard < 20000 && w.sim.find(w.enemy)->alive && w.sim.find(w.enemy)->hp > hp_at_most;
         ++guard) {
        w.sim.step();
    }
    w.options.seed = kSeed;
    w.options.radius_tiles = 20;
    w.options.extent = kFrame;
    w.options.viewer_unit = w.sim.units().front().id;
    w.options.camera = Camera{100.0F, 14.0F, 76.0F, 0.0F, -0.4F};
    w.options.show_ledger = false;
    return w;
}

/// Two vertices are the same vertex when every MEMBER is the same. `memcmp` over the struct reads
/// its object representation instead, which is not unique for floats (and would read padding if the
/// struct ever grew any), so a byte comparison is not a statement about the geometry.
[[nodiscard]] bool same_vertex(const SceneVertex& left, const SceneVertex& right) {
    return left.position == right.position && left.colour == right.colour
           && left.alpha == right.alpha && left.u == right.u && left.v == right.v;
}

/// The vertices a with-target scene has that the bare scene does not: the common head and the
/// common tail are cut away and what is left is the insertion.
[[nodiscard]] std::pair<std::size_t, std::size_t> inserted(const Scene& bare, const Scene& with) {
    std::size_t head = 0;
    const std::size_t shortest = std::min(bare.vertices.size(), with.vertices.size());
    while (head < shortest && same_vertex(bare.vertices.at(head), with.vertices.at(head))) {
        ++head;
    }
    std::size_t tail = 0;
    while (tail < shortest - head
           && same_vertex(bare.vertices.at(bare.vertices.size() - 1 - tail),
                          with.vertices.at(with.vertices.size() - 1 - tail))) {
        ++tail;
    }
    return {head, with.vertices.size() - tail};
}

struct Readout {
    Rect plate;
    std::size_t glyph_quads{};
    Rect track;
    Rect fill;
    std::array<float, 3> fill_colour{};
    std::array<float, 3> track_colour{};
    std::array<float, 3> glyph_colour{};
};

[[nodiscard]] Readout read_readout(const Watch& w, const Scene& bare, const Scene& with) {
    const auto [first, last] = inserted(bare, with);
    Readout r;
    const std::size_t quads = (last - first) / 6;
    EXPECT_EQ((last - first) % 6, 0U);
    EXPECT_GE(quads, 4U) << "a plate, at least one glyph, a track and a fill";
    r.plate = rect_of(w.options, with, first);
    r.glyph_quads = quads - 3;
    r.glyph_colour = with.vertices.at(first + 6).colour;
    r.track = rect_of(w.options, with, last - 12);
    r.track_colour = with.vertices.at(last - 12).colour;
    r.fill = rect_of(w.options, with, last - 6);
    r.fill_colour = with.vertices.at(last - 6).colour;
    return r;
}

[[nodiscard]] Scene scene_of(const Watch& w, bool target) {
    SceneOptions options = w.options;
    if (target) {
        options.attack_target = w.enemy;
    }
    return build_scene(w.sim, options);
}

[[nodiscard]] float fraction_of(const Watch& w) {
    const sim::Unit* enemy = w.sim.find(w.enemy);
    return static_cast<float>(enemy->hp) / static_cast<float>(sim::spec_of(enemy->kind).hp);
}

} // namespace

TEST(AttackReadout, WithNoTargetTheFrameIsByteIdenticalToOneThatNeverHeardOfIt) {
    const Watch w = watch_enemy(1000);
    SceneOptions plain = w.options;
    const Scene a = build_scene(w.sim, plain);
    plain.attack_target = UnitId{};
    const Scene b = build_scene(w.sim, plain);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)),
        0);
}

TEST(AttackReadout, ALabelPlateAndABarAreDrawnForTheTarget) {
    const Watch w = watch_enemy(1000);
    const Scene bare = scene_of(w, false);
    const Scene with = scene_of(w, true);
    ASSERT_GT(with.vertices.size(), bare.vertices.size());
    const Readout r = read_readout(w, bare, with);
    EXPECT_GT(r.glyph_quads, 0U);
    EXPECT_EQ(r.glyph_colour, (std::array<float, 3>{kLabelInk, kLabelInk, kLabelInk}));
}

TEST(AttackReadout, TheBarIs48By5AtEightHundredByFourFifty) {
    const Watch w = watch_enemy(1000);
    const Readout r = read_readout(w, scene_of(w, false), scene_of(w, true));
    EXPECT_NEAR(r.track.width(), 48.0F, 0.1F);
    EXPECT_NEAR(r.track.height(), 5.0F, 0.1F);
}

TEST(AttackReadout, TheLabelIsAboveTheBarAndBothAreCentredOverTheUnit) {
    const Watch w = watch_enemy(1000);
    const Readout r = read_readout(w, scene_of(w, false), scene_of(w, true));
    const sim::Unit* enemy = w.sim.find(w.enemy);
    const Projected top = project(w.options.camera, w.options.extent,
                                  static_cast<float>(enemy->x.raw) / 65536.0F * kMetresPerTile,
                                  ground_metres(kSeed, enemy->x, enemy->y) + 1.2F,
                                  static_cast<float>(enemy->y.raw) / 65536.0F * kMetresPerTile);
    EXPECT_LT(r.plate.bottom, r.track.top) << "the label is over the bar";
    EXPECT_LT(r.track.bottom, top.y) << "and both over the unit";
    EXPECT_NEAR(r.track.centre_x(), top.x, 0.5F);
    EXPECT_NEAR(r.plate.centre_x(), top.x, 0.5F);
}

TEST(AttackReadout, AtFullHealthTheBarIsFullWidthAndBrightGreen) {
    const Watch w = watch_enemy(1000); // the enemy has not been touched
    ASSERT_EQ(w.sim.find(w.enemy)->hp, sim::spec_of(UnitKind::Tank).hp);
    const Readout r = read_readout(w, scene_of(w, false), scene_of(w, true));
    EXPECT_NEAR(r.fill.width(), r.track.width(), 0.01F);
    EXPECT_NEAR(r.fill.left, r.track.left, 0.01F);
    const std::array<float, 1> whole{1.0F};
    EXPECT_EQ(r.fill_colour, bar_segments(whole).front().rgb)
        << "the row's own top-of-the-ramp green, not a second green";
    EXPECT_GT(r.fill_colour.at(1), r.fill_colour.at(0)) << "and it is green";
    EXPECT_EQ(r.track_colour, (std::array<float, 3>{0.0F, 0.0F, 0.0F}))
        << "the empty part is black";
}

TEST(AttackReadout, AsHealthFallsTheBarShortensFromTheRightAndTheColourMoves) {
    std::array<std::array<float, 3>, 3> colours{};
    std::size_t at = 0;
    for (const std::uint16_t hp : {std::uint16_t{66}, std::uint16_t{40}, std::uint16_t{12}}) {
        const Watch w = watch_enemy(hp);
        ASSERT_TRUE(w.sim.find(w.enemy)->alive) << "stepped to at most " << hp;
        const float fraction = fraction_of(w);
        ASSERT_LT(fraction, 1.0F);
        const Readout r = read_readout(w, scene_of(w, false), scene_of(w, true));
        EXPECT_NEAR(r.track.width(), 48.0F, 0.1F) << "the black track never shrinks";
        EXPECT_NEAR(r.fill.left, r.track.left, 0.01F) << "the filled part is on the LEFT";
        EXPECT_NEAR(r.fill.width(), r.track.width() * fraction, 0.01F)
            << "and is cut from the right, in proportion to health";
        const std::array<float, 1> now{fraction};
        EXPECT_EQ(r.fill_colour, bar_segments(now).front().rgb);
        colours.at(at++) = r.fill_colour;
    }
    EXPECT_NE(colours.at(0), colours.at(1));
    EXPECT_NE(colours.at(1), colours.at(2));
    EXPECT_GT(colours.at(0).at(1), colours.at(2).at(1)) << "green drains away as it is hurt";
}

TEST(AttackReadout, TheRampRunsGreenYellowRedAndVeryDarkRed) {
    // The bands the row's bars already use, read through `bar_segments`: no colour is invented.
    const auto at = [](float fraction) {
        const std::array<float, 1> f{fraction};
        return bar_segments(f).front().rgb;
    };
    EXPECT_GT(at(1.0F).at(1), at(1.0F).at(0)) << "green";
    EXPECT_GT(at(0.66F).at(0), 0.9F) << "yellow: red and green both high";
    EXPECT_GT(at(0.66F).at(1), 0.7F);
    EXPECT_GT(at(0.33F).at(0), 0.9F) << "red";
    EXPECT_LT(at(0.33F).at(1), 0.3F);
    EXPECT_LT(at(0.02F).at(0), 0.3F) << "very dark red";
    EXPECT_GT(at(0.02F).at(0), at(0.02F).at(1));
}

TEST(AttackReadout, AUnitAtZeroHealthDrawsNeitherLabelNorBar) {
    const Watch w = watch_enemy(0); // stepped until it is dead
    ASSERT_FALSE(w.sim.find(w.enemy)->alive);
    const Scene bare = scene_of(w, false);
    const Scene with = scene_of(w, true);
    ASSERT_EQ(bare.vertices.size(), with.vertices.size()) << "nothing is added for a dead unit";
    EXPECT_EQ(std::memcmp(bare.vertices.data(), with.vertices.data(),
                          bare.vertices.size() * sizeof(SceneVertex)),
              0);
}

TEST(AttackReadout, ItIsAPureFunctionOfTheSimAndTheOptions) {
    const Watch w = watch_enemy(60);
    const Scene a = scene_of(w, true);
    const Scene b = scene_of(w, true);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)),
        0);
}

TEST(AttackReadout, TheLabelIsTheKindsNameFromTheRowAndNotASecondTable) {
    const Watch w = watch_enemy(1000);
    const Readout r = read_readout(w, scene_of(w, false), scene_of(w, true));
    // "TANK" is four glyphs of the 5x5 face: T, A, N, K light 9 + 12 + 11 + 10 = at most 42 cells,
    // and a name three times as long would light far more than a table that said something else.
    ASSERT_EQ(kind_label(UnitKind::Tank), "TANK");
    EXPECT_GE(r.glyph_quads, 4U);
    EXPECT_LE(r.glyph_quads, 4U * 25U);
    // The plate is as wide as the text: four glyphs, three gaps, two pads, at 2 px cells.
    EXPECT_NEAR(r.plate.width(), (((4.0F * 6.0F) - 1.0F) * 2.0F) + (2.0F * 2.0F), 0.2F);
}

namespace {

/// The menu as the scene is given it, with the line's start worked out the way the scene lays it
/// out: the HUD's 6 px margin, the notice row, the command row, all at the frame's scale.
[[nodiscard]] MenuLayout layout_for(const std::vector<MenuRow>& rows, const Extent& extent) {
    const float px = static_cast<float>(extent.height) / 720.0F;
    return menu_layout(rows, 6.0F * px, 65.0F * px, 2.0F * px);
}

[[nodiscard]] std::vector<MenuRow> attack_rows() {
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.attack = true;
    return menu_rows(in);
}

[[nodiscard]] std::vector<std::size_t> line_vertices(const Scene& scene) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        if (scene.vertices.at(i).colour == kAttackLineInk) {
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

TEST(AttackLine, ARedLineRunsFromTheSpaceAttackRowToTheExactPositionOfTheEnemy) {
    Watch w = watch_enemy(1000);
    const std::vector<MenuRow> rows = attack_rows();
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.attack_target = w.enemy;
    const Scene scene = build_scene(w.sim, w.options);

    const std::vector<std::size_t> at = line_vertices(scene);
    ASSERT_EQ(at.size(), 6U) << "one quad, in the line's own red";
    ASSERT_EQ(at.back() - at.front(), 5U) << "contiguous";
    // emit_quad's winding: a b c, a c d. a and d are the start edge, b and c the end edge.
    const std::array<float, 2> a = pixel_of(w.options, scene.vertices.at(at.at(0)));
    const std::array<float, 2> b = pixel_of(w.options, scene.vertices.at(at.at(1)));
    const std::array<float, 2> c = pixel_of(w.options, scene.vertices.at(at.at(2)));
    const std::array<float, 2> d = pixel_of(w.options, scene.vertices.at(at.at(5)));
    const std::array<float, 2> start{(a.at(0) + d.at(0)) * 0.5F, (a.at(1) + d.at(1)) * 0.5F};
    const std::array<float, 2> end{(b.at(0) + c.at(0)) * 0.5F, (b.at(1) + c.at(1)) * 0.5F};

    const std::array<float, 2> origin = attack_line_origin(layout_for(rows, w.options.extent));
    EXPECT_NEAR(start.at(0), origin.at(0), 0.05F) << "the right edge of the SPACE Attack bar";
    EXPECT_NEAR(start.at(1), origin.at(1), 0.05F) << "at the bar's vertical middle";
    const Projected target = attack_point(w.options, *w.sim.find(w.enemy));
    EXPECT_NEAR(end.at(0), target.x, 0.05F) << "and it ends ON the enemy";
    EXPECT_NEAR(end.at(1), target.y, 0.05F);
    EXPECT_NEAR(std::hypot(a.at(0) - d.at(0), a.at(1) - d.at(1)), 2.0F * (450.0F / 720.0F), 0.05F)
        << "as thick as the menu's own leader";
}

TEST(AttackLine, TheLineStartsAtTheRightEdgeOfTheFirstRowsVerbBar) {
    const std::vector<MenuRow> rows = attack_rows();
    const MenuLayout layout = menu_layout(rows, 6.0F, 65.0F, 2.0F);
    const std::array<float, 2> origin = attack_line_origin(layout);
    EXPECT_FLOAT_EQ(origin.at(0), layout.rows.front().verb_right);
    EXPECT_FLOAT_EQ(origin.at(1), layout.rows.front().top + (layout.rows.front().height * 0.5F));
}

TEST(AttackLine, ItIsGoneTheMomentTheTargetIsNotOffered) {
    Watch w = watch_enemy(1000);
    const std::vector<MenuRow> rows = attack_rows();
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.attack_target = UnitId{};
    EXPECT_TRUE(line_vertices(build_scene(w.sim, w.options)).empty());
}

TEST(AttackLine, AMenuThatDoesNotSayAttackDrawsNoLine) {
    Watch w = watch_enemy(1000);
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    const std::vector<MenuRow> rows = menu_rows(in); // SPACE with no verb: no offer
    w.options.show_menu = true;
    w.options.menu = &rows;
    w.options.attack_target = w.enemy;
    EXPECT_TRUE(line_vertices(build_scene(w.sim, w.options)).empty())
        << "a line never points from a menu that is not offering the attack";
}

} // namespace protekt::game
