// MANEUVERS: the app layer's missing invariant, as scripted interactions (N13).
//
// The layers are pinned, and so are the sim and the drawing. The input and menu path -- keys in,
// orders and an open or closed menu out -- was pinned only a part at a time. A maneuver is a SCRIPT
// (keys at frames) and an ASSERTION on state afterwards: the order a unit carries, whether the menu
// is open, whether a placement is still armed and where its preview falls. No pixels are compared;
// the capture that goes with `AnArmedDeploymentSurvivesTheCameraKeys` is for a person to look at.
//
// The `Field` below does what `Game::command` does for a frame, in the same order -- the nav chain
// sees the keys first, then the session turns what is left into orders, which are enqueued, then
// the sim steps -- and reads the menu through `app::menu_for`, which is what the game draws. The
// glue is a dozen lines and is deliberately not a framework.
//
// The capture for `AnArmedDeploymentSurvivesTheCameraKeys` (the ghost on the ground after the
// camera has moved; F7 is the undeployed recycler's key on the default scenario, W is held by
// --autodrive):
//   protekt-game --headless --frames 30 --autodrive --key 2 f7 --digit 3 3 --capture armed.ppm
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/aim.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {
namespace {

using sim::Side;
using sim::Stance;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};
/// Where the reticle sits for these maneuvers: the middle of the screen's width and well below the
/// horizon, so its ray meets the ground a short way in front of the hull.
constexpr game::Aim kReticle{640.0F, 520.0F};

class Field {
public:
    explicit Field(const std::vector<sim::Spawn>& spawns, std::uint16_t ore = 0)
        : sim_(sim::Sim::create(with_ore(spawns, ore))) {
        for (const sim::Unit& unit : sim_.units()) {
            if (unit.side == Side::Player && !driven_.valid()) {
                driven_ = unit.id; // the first player spawn is the hull being driven
            }
        }
        session_ = game::Session{driven_};
        row_.seed_from_start(sim_, Side::Player, marks_.marks().size(), driven_);
    }

    static sim::Scenario with_ore(const std::vector<sim::Spawn>& spawns, std::uint16_t ore) {
        sim::Scenario scenario{kSeed, spawns};
        scenario.start_ore = ore;
        scenario.store_capacity = ore > 0 ? 400 : scenario.store_capacity;
        return scenario;
    }

    /// One frame: `keys` are PRESSED this frame; the ones in `held_` are pressed on every frame,
    /// the way `--autodrive` holds W.
    void frame(const std::vector<std::int32_t>& keys = {}) {
        input::InputFrame hands;
        for (const std::int32_t held : held_) {
            hands.keys.push_back(input::KeyEvent{held, true, false});
        }
        for (const std::int32_t key : keys) {
            hands.keys.push_back(input::KeyEvent{key, true, false});
        }
        step(hands);
    }

    void step(const input::InputFrame& hands, bool tick = true) {
        game::SceneOptions options = scene_options();
        const bool top_menu = !session_.go_armed() && !session_.attack_armed()
                              && !session_.tilde_steps()
                              && menu_for(sim_, session_, options, session_.attack_armed()).empty();
        input::InputFrame rest = hands;
        rest.keys = nav_.handle(hands.keys, marks_, game::nav_aim(sim_, options), top_menu);
        std::vector<sim::Order> orders;
        session_.frame(rest, sim_, options, orders);
        for (const sim::Order& order : orders) {
            sim_.enqueue(order);
            // The hull being driven is ordered every frame (its throttle) and told once that it is
            // the player's: those are driving, not commands, and are not what a maneuver is about.
            if (order.unit != driven_ && order.kind() != sim::OrderKind::TakeControl) {
                orders_.push_back(order);
            }
        }
        if (tick) {
            sim_.step();
        }
        row_.update(sim_, Side::Player, marks_.marks().size(), driven_, session_.held_batch());
    }

    /// A frame the 30 Hz clock does NOT tick on (N17): at 100 fps that is two frames in three. The
    /// window delivers its mouse motion and the session spends it; the sim stays where it was.
    void look_between_ticks(float dx, float dy = 0.0F) {
        input::InputFrame hands;
        hands.mouse_dx = dx;
        hands.mouse_dy = dy;
        step(hands, false);
    }
    /// The camera the game would draw this frame from: the same `camera_for` the scene uses.
    [[nodiscard]] game::Camera camera() const {
        return camera_for(sim_, session_, kSeed, std::nullopt);
    }
    [[nodiscard]] sim::Tick tick() const { return sim_.tick(); }
    [[nodiscard]] std::uint64_t hash() const { return sim_.state_hash(); }

    /// One frame as the WINDOW delivers it (render/window.cpp): a key goes DOWN once, then nothing
    /// is said until it goes UP -- no event on the frames between -- and the mouse's relative
    /// motion rides the same frame. `--autodrive` is not this: it re-presses W on every frame and
    /// never releases it, and a pointer's motion is something it never carries.
    void real_frame(const std::vector<input::KeyEvent>& events, float look_dx = 0.0F,
                    float look_dy = 0.0F) {
        input::InputFrame hands;
        hands.keys = events;
        hands.mouse_dx = look_dx;
        hands.mouse_dy = look_dy;
        step(hands);
    }
    void key_down(std::int32_t key) { real_frame({input::KeyEvent{key, true, false}}); }
    void key_up(std::int32_t key) { real_frame({input::KeyEvent{key, false, false}}); }
    void look(float dx, float dy = 0.0F) { real_frame({}, dx, dy); }

    void press(std::int32_t key) { frame({key}); }
    void hold(std::int32_t key) { held_.push_back(key); }
    void release_all() { held_.clear(); }

    /// Select what a function key holds -- the way a selection is made in play.
    void select(sim::UnitId unit) {
        for (std::size_t slot = 0; slot < row_.slots.size(); ++slot) {
            const auto& units = row_.slots.at(slot).units;
            if (std::find(units.begin(), units.end(), unit) != units.end()
                && slot >= game::group_slot(1)) {
                press(input::kKeyF1 + static_cast<int>(slot - game::group_slot(1)));
                return;
            }
        }
        FAIL() << "no function key holds that unit";
    }

    [[nodiscard]] game::SceneOptions scene_options() const {
        game::SceneOptions options;
        options.seed = kSeed;
        options.extent = kExtent;
        options.camera = camera_for(sim_, session_, kSeed, std::nullopt);
        options.aim = kReticle;
        options.viewer_unit = driven_;
        options.driven_unit = driven_;
        options.nav_marks = &marks_;
        options.row = &row_;
        return options;
    }

    [[nodiscard]] std::vector<game::MenuRow> menu() const {
        return menu_for(sim_, session_, scene_options(), session_.attack_armed());
    }
    /// The menu is on screen when it has any row: nothing selected draws none.
    [[nodiscard]] bool menu_open() const { return !menu().empty(); }
    [[nodiscard]] game::NavAim aim_at_ground() const {
        return game::nav_aim(sim_, scene_options());
    }
    [[nodiscard]] const sim::Unit& unit(sim::UnitId id) const { return *sim_.find(id); }
    [[nodiscard]] const sim::Unit& first(UnitKind kind) const {
        for (const sim::Unit& candidate : sim_.units()) {
            if (candidate.kind == kind && candidate.side == Side::Player) {
                return candidate;
            }
        }
        ADD_FAILURE() << "the field has no such unit";
        return *sim_.units().begin();
    }
    [[nodiscard]] std::vector<sim::UnitId> all(UnitKind kind) const {
        std::vector<sim::UnitId> ids;
        for (const sim::Unit& candidate : sim_.units()) {
            if (candidate.kind == kind && candidate.side == Side::Player
                && candidate.id != driven_) {
                ids.push_back(candidate.id);
            }
        }
        return ids;
    }
    [[nodiscard]] const sim::Unit& hull() const { return *sim_.find(driven_); }
    [[nodiscard]] const game::Session& session() const { return session_; }
    /// The function keys, as the game holds them (the same `RowState` `Game::refresh_row` fills).
    [[nodiscard]] const game::RowState& row() const { return row_; }
    /// How many of F1..F10 are on the screen: they hold a unit, or are held for a batch.
    [[nodiscard]] std::size_t function_keys() const {
        std::size_t taken = 0;
        for (std::size_t slot = game::kRowFirstGroup; slot < game::kRowSlots; ++slot) {
            taken += row_.occupied(slot) ? 1U : 0U;
        }
        return taken;
    }
    [[nodiscard]] sim::UnitId driven() const { return driven_; }
    [[nodiscard]] const std::vector<sim::Order>& orders() const { return orders_; }

private:
    sim::Sim sim_;
    game::Session session_{sim::UnitId{}};
    game::NavMarks marks_;
    game::NavMenu nav_;
    mutable game::RowState row_;
    sim::UnitId driven_{};
    std::vector<std::int32_t> held_;
    std::vector<sim::Order> orders_; ///< every order the script gave, in the order it gave them
};

[[nodiscard]] sim::Spawn spawn(UnitKind kind, Side side, int x, int y) {
    sim::Spawn spawn{};
    spawn.kind = kind;
    spawn.side = side;
    spawn.tile_x = x;
    spawn.tile_y = y;
    return spawn;
}

/// The driven tank, a hover-less lot of wingmen and a lone rocket tank, an undeployed recycler, and
/// an enemy well out of the way.
[[nodiscard]] Field make_field() {
    return Field({spawn(UnitKind::Tank, Side::Player, 40, 40),
                  spawn(UnitKind::Tank, Side::Player, 44, 41),
                  spawn(UnitKind::Tank, Side::Player, 36, 41),
                  spawn(UnitKind::RocketTank, Side::Player, 42, 44),
                  spawn(UnitKind::Recycler, Side::Player, 36, 46),
                  spawn(UnitKind::Tank, Side::Enemy, 120, 120)});
}

[[nodiscard]] bool has_row(const std::vector<game::MenuRow>& rows, std::string_view verb) {
    return std::any_of(rows.begin(), rows.end(),
                       [&](const game::MenuRow& row) { return row.verb == verb; });
}

constexpr std::int32_t kDigit1 = input::kKey1;
constexpr std::int32_t kDigit3 = input::kKey1 + 2;

} // namespace

// ------------------------------------------------------------------ follow closes the menu

TEST(Maneuver, FollowClosesTheMenuForOneUnit) {
    // Script: frame 1 the key that holds the lone rocket tank (it is selected, the menu opens),
    // frame 2 `1` (Follow Me). The tank follows the driven hull and the menu is gone.
    Field field = make_field();
    const sim::UnitId rocket = field.first(UnitKind::RocketTank).id;
    field.select(rocket);
    ASSERT_TRUE(field.menu_open()) << "selecting a vehicle opens its menu";
    ASSERT_TRUE(has_row(field.menu(), game::kVerbFollowMe));

    field.press(kDigit1);

    EXPECT_EQ(field.unit(rocket).stance, Stance::Follow) << "the order is on that unit";
    EXPECT_EQ(field.unit(rocket).target, field.driven());
    EXPECT_FALSE(field.menu_open()) << "the command is done: the menu goes away";
    EXPECT_TRUE(field.session().selection().units().empty());
}

TEST(Maneuver, FollowClosesTheMenuForAGroup) {
    // The same, with the tanks' function key: a group of two wingmen (the driven hull is never
    // ordered). EVERY ordered unit carries the order, and the menu is closed.
    Field field = make_field();
    const std::vector<sim::UnitId> wingmen = field.all(UnitKind::Tank);
    ASSERT_EQ(wingmen.size(), 2U);
    field.select(wingmen.front());
    ASSERT_EQ(field.session().selection().size(), 2U) << "the key holds the whole group";
    ASSERT_TRUE(field.menu_open());

    field.press(kDigit1);

    for (const sim::UnitId id : wingmen) {
        EXPECT_EQ(field.unit(id).stance, Stance::Follow) << "unit " << id.value;
        EXPECT_EQ(field.unit(id).target, field.driven());
    }
    EXPECT_NE(field.unit(field.driven()).stance, Stance::Follow) << "nobody orders the driven hull";
    EXPECT_FALSE(field.menu_open()) << "the group's menu closes too";
    EXPECT_TRUE(field.session().selection().units().empty());
}

// ------------------------------------------------------------------ the armed deployment

TEST(Maneuver, AnArmedDeploymentSurvivesTheCameraKeys) {
    // Script: select the undeployed recycler, `3` Deploy (arms it), then hold W for a while, then
    // turn with A, then D, then back with S. The deployment stays armed, the menu stays open with
    // its Deploy state row, the reticle still meets the ground, and the tile it meets MOVED with
    // the camera -- so the preview tracks. SPACE then deploys at the tile the preview showed.
    Field field = make_field();
    const sim::UnitId recycler = field.first(UnitKind::Recycler).id;
    field.select(recycler);
    field.press(kDigit3);
    ASSERT_TRUE(field.session().deploy_armed()) << "`3` on an undeployed recycler arms the deploy";
    const game::NavAim before = field.aim_at_ground();
    ASSERT_TRUE(before.on_ground());

    field.hold(input::kKeyW);
    for (int i = 0; i < 20; ++i) {
        field.frame();
        ASSERT_TRUE(field.session().deploy_armed())
            << "W dropped the armed deployment, frame " << i;
    }
    field.release_all();
    field.hold(input::kKeyA);
    for (int i = 0; i < 12; ++i) {
        field.frame();
        ASSERT_TRUE(field.session().deploy_armed())
            << "A dropped the armed deployment, frame " << i;
    }
    field.release_all();
    field.hold(input::kKeyD);
    for (int i = 0; i < 6; ++i) {
        field.frame();
        ASSERT_TRUE(field.session().deploy_armed())
            << "D dropped the armed deployment, frame " << i;
    }
    field.release_all();
    field.hold(input::kKeyS);
    for (int i = 0; i < 4; ++i) {
        field.frame();
        ASSERT_TRUE(field.session().deploy_armed())
            << "S dropped the armed deployment, frame " << i;
    }
    field.release_all();

    EXPECT_TRUE(field.menu_open()) << "the menu is still up while the deploy is armed";
    const game::NavAim after = field.aim_at_ground();
    ASSERT_TRUE(after.on_ground()) << "the reticle can still point at the ground";
    EXPECT_TRUE(after.tile_x != before.tile_x || after.tile_y != before.tile_y)
        << "the camera moved, so the tile under the reticle must have moved with it";

    // And the preview is not a decoration: SPACE deploys exactly where the preview stood.
    const std::size_t given = field.orders().size();
    field.press(input::kKeySpace);
    const std::vector<sim::Order> fresh(field.orders().begin() + static_cast<std::ptrdiff_t>(given),
                                        field.orders().end());
    ASSERT_EQ(fresh.size(), 1U) << "SPACE gave the deploy";
    ASSERT_EQ(fresh.front().kind(), sim::OrderKind::Deploy);
    const auto& deploy = std::get<sim::orders::Deploy>(fresh.front().payload);
    EXPECT_EQ(deploy.tile_x, after.tile_x);
    EXPECT_EQ(deploy.tile_y, after.tile_y);
    EXPECT_FALSE(field.session().deploy_armed());
    EXPECT_FALSE(field.menu_open()) << "the click completes the sequence";
}

// ------------------------------------------------------------------ moving AND placing (N14)
//
// Harri's rule: wherever he points, that is where the deployment goes; the movement keys do not
// cancel the placement, and because the site is where the reticle meets the ground, moving the
// view moves the site. Both clauses, driven the way the WINDOW drives (`Field::key_down` and
// `key_up`, `Field::look`), not the way `--autodrive` does.

namespace {

[[nodiscard]] Field armed_field() {
    Field field = make_field();
    field.select(field.first(UnitKind::Recycler).id);
    field.press(kDigit3);
    return field;
}

} // namespace

TEST(Maneuver, MovingTheViewWhileArmedKeepsItArmed) {
    Field field = armed_field();
    ASSERT_TRUE(field.session().deploy_armed());
    const game::NavAim before = field.aim_at_ground();
    ASSERT_TRUE(before.on_ground());

    for (int i = 0; i < 10; ++i) {
        field.look(40.0F, 0.0F);
        ASSERT_TRUE(field.session().deploy_armed()) << "looking dropped the deployment, " << i;
    }

    const game::NavAim after = field.aim_at_ground();
    ASSERT_TRUE(after.on_ground());
    EXPECT_TRUE(after.tile_x != before.tile_x || after.tile_y != before.tile_y)
        << "the view turned, so the ground under the reticle -- the site -- moved with it";
}

TEST(Maneuver, DrivingWithTheMovementKeysKeepsItArmed) {
    Field field = armed_field();
    const sim::Unit start = field.hull();
    for (const std::int32_t key : {input::kKeyW, input::kKeyA, input::kKeyD, input::kKeyS}) {
        field.key_down(key);
        for (int i = 0; i < 15; ++i) {
            field.frame();
            ASSERT_TRUE(field.session().deploy_armed()) << "key " << key << " dropped it, " << i;
        }
        field.key_up(key);
        ASSERT_TRUE(field.session().deploy_armed()) << "releasing key " << key << " dropped it";
    }
    EXPECT_TRUE(field.menu_open());
    EXPECT_TRUE(field.hull().x.raw != start.x.raw || field.hull().y.raw != start.y.raw)
        << "the hull really drove: the test is not vacuous";
}

TEST(Maneuver, TheSiteIsWhereTheReticleMeetsTheGroundNotWhereTheHullIs) {
    Field field = armed_field();
    field.key_down(input::kKeyW);
    for (int i = 0; i < 20; ++i) {
        field.frame();
    }
    field.key_up(input::kKeyW);
    for (int i = 0; i < 8; ++i) {
        field.look(50.0F, 0.0F); // turn the view away from where the hull is heading
    }
    ASSERT_TRUE(field.session().deploy_armed());
    const game::NavAim pointed = field.aim_at_ground();
    ASSERT_TRUE(pointed.on_ground());

    const std::size_t given = field.orders().size();
    field.press(input::kKeySpace);

    ASSERT_EQ(field.orders().size(), given + 1) << "SPACE gave the deploy";
    const auto& deploy = std::get<sim::orders::Deploy>(field.orders().back().payload);
    EXPECT_EQ(deploy.tile_x, pointed.tile_x) << "the deployment lands at the pointed spot";
    EXPECT_EQ(deploy.tile_y, pointed.tile_y);
    const int hull_x = field.hull().x.floor_int();
    const int hull_y = field.hull().y.floor_int();
    EXPECT_TRUE(deploy.tile_x != hull_x || deploy.tile_y != hull_y)
        << "and it is not under the hull";
}

TEST(Maneuver, ArmingIsStillRefusedOnBadGround) {
    // The recycler's own footprint is not clear ground for a second one: aim at the recycler's
    // tile and SPACE is refused, the deployment stays armed, no order is given.
    Field field = armed_field();
    const std::size_t given = field.orders().size();
    for (int i = 0; i < 40 && field.session().deploy_armed(); ++i) {
        field.look(-30.0F, 0.0F);
        field.press(input::kKeySpace);
        if (field.orders().size() != given) {
            break;
        }
    }
    // Whatever the reticle crossed, a refusal leaves the arming alone and an acceptance disarms it
    // after exactly one Deploy: never an order with the arming still up.
    EXPECT_EQ(field.orders().size() != given, !field.session().deploy_armed());
}

// ------------------------------------------------------------------ the shift batch (N15)
//
// Harri: "Select the deployed recycler with 1, hold shift, hit 1 twice to build two scavengers,
// let go of shift. Does the menu go away? It should." Driven as the WINDOW drives: shift is a key
// event of its own (down, then up) and every key pressed meanwhile carries the shift flag.
// (`--key`/`--autodrive` cannot do this: they never set the flag and have no shift key to name.)

TEST(Maneuver, TheShiftBatchBuildClosesTheMenu) {
    Field field({spawn(UnitKind::Tank, Side::Player, 40, 40),
                 spawn(UnitKind::DeployedRecycler, Side::Player, 46, 41),
                 spawn(UnitKind::Tank, Side::Enemy, 120, 120)},
                200);
    const sim::UnitId recycler = field.first(UnitKind::DeployedRecycler).id;

    field.press(kDigit1); // `1` brings up the deployed recycler
    ASSERT_TRUE(field.menu_open()) << "selecting the recycler opens its menu";

    field.real_frame({input::KeyEvent{input::kKeyLShift, true, true}});
    field.real_frame({input::KeyEvent{kDigit1, true, true}});
    field.real_frame({input::KeyEvent{kDigit1, true, true}});
    EXPECT_EQ(field.unit(recycler).queue.count, 0U)
        << "held: nothing is ordered while shift is down";
    EXPECT_TRUE(field.orders().empty());

    field.real_frame({input::KeyEvent{input::kKeyLShift, false, false}});

    std::size_t builds = 0;
    for (const sim::Order& order : field.orders()) {
        if (order.kind() == sim::OrderKind::Build) {
            ++builds;
            EXPECT_EQ(std::get<sim::orders::Build>(order.payload).kind, UnitKind::Scavenger);
        }
    }
    EXPECT_EQ(builds, 2U) << "two presses, two scavengers -- not one, not three";
    EXPECT_EQ(field.unit(recycler).queue.count, 2U) << "and both are in the recycler's queue";
    EXPECT_FALSE(field.menu_open()) << "the batch is begun: the menu goes away";
    EXPECT_TRUE(field.session().selection().units().empty());
}

// ------------------------------------------------------------------ the construct mode (N16)
//
// Harri: "Select the Constructor, `3 Construct`, `1 Power`, WASD should not take you back to the
// previous menu -- you should stay in the construct-Power mode." The rule, from three maneuvers:
// THE MOVEMENT KEYS NEVER INTERRUPT A PLACEMENT OR CONSTRUCT MODE.

TEST(Maneuver, WASDKeepsTheConstructMode) {
    for (const std::int32_t key : {input::kKeyW, input::kKeyA, input::kKeyS, input::kKeyD}) {
        Field field({spawn(UnitKind::Tank, Side::Player, 40, 40),
                     spawn(UnitKind::Constructor, Side::Player, 44, 41),
                     spawn(UnitKind::Tank, Side::Enemy, 120, 120)},
                    400);
        field.select(field.first(UnitKind::Constructor).id);
        field.press(kDigit3);
        field.press(kDigit1);
        // The mode IS: the panel up, on a Constructor, with the Power plant armed.
        const auto expect_power_mode = [&](const char* when) {
            EXPECT_TRUE(field.session().construct_armed()) << "key " << key << ' ' << when;
            ASSERT_TRUE(field.session().placing().has_value()) << "key " << key << ' ' << when;
            EXPECT_EQ(field.session().placing()->kind, UnitKind::Power)
                << "key " << key << ' ' << when;
            EXPECT_EQ(field.session().selection().size(), 1U) << "key " << key << ' ' << when;
            EXPECT_TRUE(field.menu_open()) << "key " << key << ' ' << when;
            const auto rows = field.menu();
            EXPECT_TRUE(std::any_of(rows.begin(), rows.end(),
                                    [](const game::MenuRow& row) {
                                        return row.heading && row.verb == game::kConstructHeading;
                                    }))
                << "key " << key << ' ' << when << ": the CONSTRUCT panel is what is drawn";
        };
        ASSERT_NO_FATAL_FAILURE(expect_power_mode("before any movement"));
        field.key_down(key); // PRESS
        ASSERT_NO_FATAL_FAILURE(expect_power_mode("on press"));
        for (int i = 0; i < 10; ++i) {
            field.frame(); // HELD: no event between down and up, as the window delivers it
        }
        ASSERT_NO_FATAL_FAILURE(expect_power_mode("while held"));
        field.key_up(key); // RELEASE
        ASSERT_NO_FATAL_FAILURE(expect_power_mode("on release"));
        EXPECT_TRUE(field.orders().empty()) << "key " << key << ": movement ordered nothing";
    }
}

// ------------------------------------------------------------------ the reservation (N19)
//
// Harri: "the sequence `1` -> shift down -> `1` [should] produce a new function key, but the build
// should not have started. It must wait for the release of the shift key. Also `1` -> shift down ->
// `1` -> tab -> shift up should produce no change. No build, no new function key." The observable
// is the function-key list itself (`RowState`, read through `Field::function_keys`), never the HUD.

namespace {

[[nodiscard]] Field recycler_field() {
    return Field({spawn(UnitKind::Tank, Side::Player, 40, 40),
                  spawn(UnitKind::DeployedRecycler, Side::Player, 46, 41),
                  spawn(UnitKind::Tank, Side::Enemy, 120, 120)},
                 200);
}

[[nodiscard]] std::size_t builds_ordered(const Field& field) {
    return static_cast<std::size_t>(
        std::count_if(field.orders().begin(), field.orders().end(), [](const sim::Order& order) {
            return order.kind() == sim::OrderKind::Build;
        }));
}

void shift_down(Field& field) {
    field.real_frame({input::KeyEvent{input::kKeyLShift, true, true}});
}
void shift_up(Field& field) {
    field.real_frame({input::KeyEvent{input::kKeyLShift, false, false}});
}

} // namespace

TEST(Maneuver, ABareBuildQueuesOneUnitAndTakesANewFunctionKey) {
    Field field = recycler_field();
    const sim::UnitId recycler = field.first(UnitKind::DeployedRecycler).id;
    const std::size_t keys_before = field.function_keys();

    field.press(kDigit1);
    field.press(kDigit1);

    EXPECT_FALSE(field.menu_open());
    EXPECT_EQ(builds_ordered(field), 1U);
    EXPECT_EQ(field.unit(recycler).queue.count, 1U) << "the order is QUEUED, once";
    EXPECT_EQ(field.function_keys(), keys_before + 1) << "a new function key for the new scavenger";
}

TEST(Maneuver, AHeldBatchTakesItsFunctionKeyOnThePressAndBuildsNothingUntilTheRelease) {
    Field field = recycler_field();
    const sim::UnitId recycler = field.first(UnitKind::DeployedRecycler).id;
    const std::size_t keys_before = field.function_keys();
    const std::size_t queued_before = field.unit(recycler).queue.count;

    field.press(kDigit1);
    shift_down(field);
    field.real_frame({input::KeyEvent{kDigit1, true, true}});

    EXPECT_EQ(field.function_keys(), keys_before + 1)
        << "the reservation happens ON THE PRESS: a new function key exists";
    EXPECT_EQ(builds_ordered(field), 0U) << "no build has started";
    EXPECT_EQ(field.unit(recycler).queue.count, queued_before) << "nothing is queued";
    for (int i = 0; i < 5; ++i) {
        field.frame(); // shift still down: still nothing, and still reserved
    }
    EXPECT_EQ(builds_ordered(field), 0U);
    EXPECT_EQ(field.function_keys(), keys_before + 1);

    shift_up(field);
    EXPECT_EQ(builds_ordered(field), 1U) << "the release is what starts the build";
    EXPECT_EQ(field.unit(recycler).queue.count, 1U);
    EXPECT_EQ(field.function_keys(), keys_before + 1) << "the reservation became the batch's key";
}

TEST(Maneuver, ACancelledBatchBuildsNothingAndLeavesNoFunctionKey) {
    Field field = recycler_field();
    const sim::UnitId recycler = field.first(UnitKind::DeployedRecycler).id;
    const std::size_t keys_before = field.function_keys();

    field.press(kDigit1);
    shift_down(field);
    field.real_frame({input::KeyEvent{kDigit1, true, true}});
    field.real_frame({input::KeyEvent{input::kKeyTab, true, true}});
    EXPECT_EQ(field.function_keys(), keys_before)
        << "the tab releases the reservation BEFORE the shift goes up";
    EXPECT_EQ(builds_ordered(field), 0U);

    shift_up(field);
    EXPECT_EQ(builds_ordered(field), 0U) << "no build";
    EXPECT_EQ(field.unit(recycler).queue.count, 0U);
    EXPECT_EQ(field.function_keys(), keys_before) << "no new function key";
    for (int i = 0; i < 5; ++i) {
        field.frame();
    }
    EXPECT_EQ(field.function_keys(), keys_before);
}

TEST(Maneuver, ASecondShiftPressWhileOneIsPendingDoesNotReserveASecondKey) {
    Field field = recycler_field();
    const std::size_t keys_before = field.function_keys();

    field.press(kDigit1);
    shift_down(field);
    field.real_frame({input::KeyEvent{kDigit1, true, true}});
    field.real_frame({input::KeyEvent{kDigit1, true, true}});

    EXPECT_EQ(field.function_keys(), keys_before + 1) << "a batch of two shares ONE key";
    EXPECT_EQ(builds_ordered(field), 0U);
    shift_up(field);
    EXPECT_EQ(builds_ordered(field), 2U);
    EXPECT_EQ(field.function_keys(), keys_before + 1);
}

// ------------------------------------------------------------------ a build row closes the menu
// (N18)
//
// Harri: "When you have a deployed recycler, doing `1` for the recycler -> `1 Scavenger` or any
// other vehicle, the menu should go away." One maneuver, the producer a parameter: select it by its
// top-menu digit, press the first row of its own panel, and the order must be QUEUED (not dropped)
// and the menu CLOSED. The Kiln and the Forge are not producers the deck can reach: nothing selects
// them and they take no menu (`menu_subject_of_kind`).

namespace {

struct Producer {
    UnitKind kind;
    std::int32_t select_key; ///< the top-menu digit that selects it
    UnitKind first_row;      ///< what row `1` of its own panel builds
};

void press_first_build_row_closes_the_menu(const Producer& producer) {
    Field field({spawn(UnitKind::Tank, Side::Player, 40, 40),
                 spawn(producer.kind, Side::Player, 46, 41),
                 spawn(UnitKind::Tank, Side::Enemy, 120, 120)},
                400);
    const sim::UnitId unit = field.first(producer.kind).id;

    field.press(producer.select_key);
    ASSERT_TRUE(field.menu_open()) << "selecting the producer opens its menu";
    ASSERT_EQ(field.session().selection().size(), 1U);
    ASSERT_TRUE(field.orders().empty());

    field.press(kDigit1);

    std::size_t builds = 0;
    for (const sim::Order& order : field.orders()) {
        if (order.kind() == sim::OrderKind::Build) {
            ++builds;
            EXPECT_EQ(std::get<sim::orders::Build>(order.payload).kind, producer.first_row);
        }
    }
    EXPECT_EQ(builds, 1U) << "the press ordered one build -- not none (ignored), not two";
    EXPECT_EQ(field.unit(unit).queue.count, 1U)
        << "and it is in the producer's queue (not dropped)";
    EXPECT_FALSE(field.menu_open()) << "the order is given: the menu goes away";
    EXPECT_TRUE(field.session().selection().units().empty());
}

} // namespace

TEST(Maneuver, PressingABuildRowClosesTheMenuFromTheDeployedRecycler) {
    press_first_build_row_closes_the_menu(
        {UnitKind::DeployedRecycler, kDigit1, UnitKind::Scavenger});
}

TEST(Maneuver, PressingABuildRowClosesTheMenuFromTheFactory) {
    press_first_build_row_closes_the_menu({UnitKind::Factory, kDigit1 + 1, UnitKind::MortarBike});
}

TEST(Maneuver, PressingAConstructRowOnTheConstructPanelKeepsThePanelAndArmsThePlacement) {
    // Not a command but a placement: the panel stays, the building is armed, nothing is ordered
    // until SPACE. (Pinned so that a change to it is a decision, not an accident.)
    Field field({spawn(UnitKind::Tank, Side::Player, 40, 40),
                 spawn(UnitKind::Constructor, Side::Player, 44, 41),
                 spawn(UnitKind::Tank, Side::Enemy, 120, 120)},
                400);
    field.select(field.first(UnitKind::Constructor).id);
    field.press(kDigit3);
    ASSERT_TRUE(field.session().construct_armed());
    field.press(kDigit1);
    EXPECT_TRUE(field.session().construct_armed());
    const auto placing = field.session().placing();
    EXPECT_EQ(placing.value_or(game::Placing{}).kind, UnitKind::Power);
    EXPECT_TRUE(field.menu_open());
    EXPECT_TRUE(field.orders().empty());
}

// ------------------------------------------------------------------ neighbours

TEST(Maneuver, ACancelledDeploymentLeavesNoOrderAndNoMenu) {
    // Tab cancels an armed deploy, Escape does too: no Deploy order is left behind, the recycler
    // is still where it was, and the menu has closed.
    for (const std::int32_t cancel : {input::kKeyTab, input::kKeyEscape}) {
        Field field = make_field();
        const sim::UnitId recycler = field.first(UnitKind::Recycler).id;
        field.select(recycler);
        field.press(kDigit3);
        ASSERT_TRUE(field.session().deploy_armed());

        field.press(cancel);

        EXPECT_FALSE(field.session().deploy_armed()) << "key " << cancel;
        EXPECT_TRUE(field.orders().empty()) << "key " << cancel << " left an order behind";
        EXPECT_NE(field.unit(recycler).stance, Stance::Deploy);
        EXPECT_FALSE(field.menu_open()) << "key " << cancel;
    }
}

TEST(Maneuver, ACancelledFollowMenuLeavesNoOrder) {
    // Select, then Tab: the menu closes and nobody was ordered.
    Field field = make_field();
    const sim::UnitId rocket = field.first(UnitKind::RocketTank).id;
    field.select(rocket);
    ASSERT_TRUE(field.menu_open());

    field.press(input::kKeyTab);

    EXPECT_FALSE(field.menu_open());
    EXPECT_TRUE(field.orders().empty());
    EXPECT_NE(field.unit(rocket).stance, Stance::Follow);
}

TEST(Maneuver, GoToNavOpensAListAndKeepsTheMenuOpen) {
    // `2 Go to Nav` is a row that opens a submenu, not an order: the selection stays, the menu
    // stays (now the list), and no order has been given. Tab then closes it with still no order.
    Field field = make_field();
    const sim::UnitId rocket = field.first(UnitKind::RocketTank).id;
    // A nav mark must exist for the row to be live: `n` then SPACE places one under the reticle,
    // and the chain opens only from the top menu, so it is done BEFORE anything is selected.
    field.press(input::kKeyN);
    field.press(input::kKeySpace);
    field.select(rocket);
    ASSERT_TRUE(has_row(field.menu(), game::kVerbGoToNav));

    field.press(input::kKey1 + 1);

    EXPECT_TRUE(field.session().nav_list_armed()) << "the row opened the nav list";
    EXPECT_TRUE(field.menu_open()) << "a row that opens a submenu must not close the menu";
    EXPECT_EQ(field.session().selection().size(), 1U);
    EXPECT_TRUE(field.orders().empty());

    field.press(input::kKeyTab);
    EXPECT_FALSE(field.menu_open());
    EXPECT_TRUE(field.orders().empty()) << "backing out of the list gave no order";
}

// ------------------------------------------------------------------ the look is per FRAME (N17)
//
// "When I look around with the mouse, it looks choppy." The first suspect was a camera that turns
// only on a sim tick, which would quantise the look to 30 Hz whatever the frame rate. These pin the
// other answer: the mouse turns the camera on EVERY frame, a tick or not, by the motion that
// arrived in that frame and no more, and the reticle's ground ray follows it the same frame.

TEST(Look, TheCameraTurnsOnAFrameThatIsNotATick) {
    Field field = make_field();
    field.look(0.0F); // settle: the first frame sets the heading
    const sim::Tick tick = field.tick();
    const float yaw = field.camera().yaw;

    field.look_between_ticks(40.0F);

    EXPECT_EQ(field.tick(), tick) << "the frame was meant to carry no tick";
    EXPECT_NE(field.camera().yaw, yaw) << "the look waited for a tick: quantised to the sim rate";
    EXPECT_NEAR(game::Camera::wrap_angle(field.camera().yaw - yaw),
                40.0F * game::kLookRadiansPerPixel, 1e-4F);
}

TEST(Look, ConsecutiveFramesBetweenTwoTicksEachTurnTheCameraByTheirOwnMotion) {
    // Quantised, the yaws across four frames with no tick would read 0 0 0 0 and then a lump;
    // per frame they step by what each frame was given. This is the number a person can read: the
    // camera's yaw at each frame.
    Field field = make_field();
    field.look(0.0F);
    const sim::Tick tick = field.tick();
    std::vector<float> yaws{field.camera().yaw};
    for (const float dx : {10.0F, 25.0F, 5.0F, 40.0F}) {
        field.look_between_ticks(dx);
        yaws.push_back(field.camera().yaw);
    }
    EXPECT_EQ(field.tick(), tick);
    const std::vector<float> expect{10.0F, 25.0F, 5.0F, 40.0F};
    for (std::size_t i = 0; i < expect.size(); ++i) {
        EXPECT_NEAR(game::Camera::wrap_angle(yaws.at(i + 1) - yaws.at(i)),
                    expect.at(i) * game::kLookRadiansPerPixel, 1e-4F)
            << "frame " << i;
    }
}

TEST(Look, AFlickSplitAcrossFramesTurnsTheCameraByTheSameTotalAsOneFrame) {
    Field in_pieces = make_field();
    Field at_once = make_field();
    in_pieces.look(0.0F);
    at_once.look(0.0F);
    const float start = in_pieces.camera().yaw;
    for (int i = 0; i < 5; ++i) {
        in_pieces.look_between_ticks(8.0F, 3.0F);
    }
    at_once.look_between_ticks(40.0F, 15.0F);
    EXPECT_NEAR(in_pieces.camera().yaw, at_once.camera().yaw, 1e-4F);
    EXPECT_NEAR(in_pieces.camera().pitch, at_once.camera().pitch, 1e-4F);
    EXPECT_NE(in_pieces.camera().yaw, start);
}

TEST(Look, TheReticlesGroundRayFollowsTheLookOnTheSameFrameWithNoTick) {
    // The aim is computed from the camera of the frame, so it must move with the look and not
    // wait for a tick either: the reticle and the picture must never disagree.
    Field field = make_field();
    field.look(0.0F);
    const game::NavAim before = field.aim_at_ground();
    ASSERT_TRUE(before.on_ground());
    const sim::Tick tick = field.tick();
    for (int i = 0; i < 10; ++i) {
        field.look_between_ticks(40.0F);
    }
    EXPECT_EQ(field.tick(), tick);
    const game::NavAim after = field.aim_at_ground();
    ASSERT_TRUE(after.on_ground());
    EXPECT_TRUE(after.tile_x != before.tile_x || after.tile_y != before.tile_y)
        << "the ground ray did not follow the look";
}

TEST(Look, AFrameThatTurnsTheViewChangesNothingTheSimHashes) {
    // A view change must not alter the sim: two fields given the same ticks, one of which also
    // looked around between them, end on the same hash.
    Field still = make_field();
    Field looking = make_field();
    still.look(0.0F);
    looking.look(0.0F);
    for (int i = 0; i < 20; ++i) {
        still.frame();
        looking.look_between_ticks(30.0F, 5.0F);
        looking.frame();
    }
    EXPECT_EQ(still.hash(), looking.hash());
}

} // namespace protekt::app
