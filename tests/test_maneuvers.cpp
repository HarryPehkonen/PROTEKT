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
    explicit Field(const std::vector<sim::Spawn>& spawns)
        : sim_(sim::Sim::create(sim::Scenario{kSeed, spawns})) {
        for (const sim::Unit& unit : sim_.units()) {
            if (unit.side == Side::Player && !driven_.valid()) {
                driven_ = unit.id; // the first player spawn is the hull being driven
            }
        }
        session_ = game::Session{driven_};
        row_.seed_from_start(sim_, Side::Player, marks_.marks().size(), driven_);
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
        sim_.step();
        row_.update(sim_, Side::Player, marks_.marks().size(), driven_);
    }

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
    [[nodiscard]] const game::Session& session() const { return session_; }
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

} // namespace protekt::app
