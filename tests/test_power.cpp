// ROUND 17, J3: POWER can be built, and it stands.
//
// The first building of the construct system, end to end: the kind and its spec row, the sim's
// `Place` order (pay, stand at once, refuse without charging), the footprint nobody may enter, the
// rotation, and the game layer's armed placement (the press arms, the preview carries the row's
// cost and footprint, SPACE places, a refused SPACE charges nothing).
//
// BUILD TIME IS NOT MODELLED HERE. The building stands the tick it is placed; the spec row carries
// the 600 ticks the other rows' 20-ticks-a-scrap rule gives, for the build-time slice to read.
// POWER SUPPLIES NOTHING YET: the power grid is the next slice.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Fixed;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

/// A block of ground, `kBlockW` x `kBlockH` tiles, that every unit may enter: found by scanning
/// the seed, so the tests do not depend on where this seed happens to put a crater.
constexpr int kBlockW = 16;
constexpr int kBlockH = 14;

struct Block {
    int x{};
    int y{};
};

[[nodiscard]] Block open_block(std::uint64_t seed) {
    for (int by = 20; by < 200; by += 2) {
        for (int bx = 20; bx < 200; bx += 2) {
            bool clear = true;
            for (int dy = 0; dy < kBlockH && clear; ++dy) {
                for (int dx = 0; dx < kBlockW && clear; ++dx) {
                    clear = sim::tile_passable(seed, bx + dx, by + dy);
                }
            }
            if (clear) {
                return Block{bx, by};
            }
        }
    }
    return Block{-1, -1};
}

/// A tile that no unit may enter (a crater), with the whole 4x2 footprint anchored on it.
[[nodiscard]] std::optional<Block> crater_tile(std::uint64_t seed) {
    for (int y = 2; y < 250; ++y) {
        for (int x = 2; x < 250; ++x) {
            if (!sim::tile_passable(seed, x, y)) {
                return Block{x, y};
            }
        }
    }
    return std::nullopt;
}

struct Yard {
    sim::Sim sim;
    Block block;
    sim::UnitId constructor;
    sim::UnitId tank;
};

/// A Constructor and a Tank on open ground, `ore` scrap banked and room in the store for it.
[[nodiscard]] Yard make_yard(std::uint16_t ore) {
    Yard y{};
    y.block = open_block(kSeed);
    sim::Scenario scenario{kSeed,
                           {{UnitKind::Constructor, Side::Player, y.block.x + 1, y.block.y + 1},
                            {UnitKind::Tank, Side::Player, y.block.x + 1, y.block.y + 5}}};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    y.sim = sim::Sim::create(scenario);
    y.constructor = y.sim.units().at(0).id;
    y.tank = y.sim.units().at(1).id;
    return y;
}

[[nodiscard]] sim::Order place(sim::UnitId who, int tile_x, int tile_y, bool turned = false) {
    return sim::Order{who, sim::orders::Place{UnitKind::Power, tile_x, tile_y, turned}};
}

[[nodiscard]] std::size_t count_of(const sim::Sim& s, UnitKind kind) {
    std::size_t n = 0;
    for (const sim::Unit& u : s.units()) {
        n += (u.alive && u.kind == kind) ? 1U : 0U;
    }
    return n;
}

[[nodiscard]] const sim::Unit* first_of(const sim::Sim& s, UnitKind kind) {
    for (const sim::Unit& u : s.units()) {
        if (u.alive && u.kind == kind) {
            return &u;
        }
    }
    return nullptr;
}

} // namespace

// ------------------------------------------------------------------------------ the kind

TEST(Power, TheKindIsAppendedWithItsSpecRowAndItsFootprint) {
    EXPECT_EQ(sim::kUnitKindCount, 22) << "the pin moves from 21 to 22: POWER is the 22nd kind";
    EXPECT_EQ(static_cast<int>(UnitKind::Power), 21) << "appended, never inserted";
    EXPECT_EQ(static_cast<int>(UnitKind::ServicePod), 20) << "no earlier value moved";
    const sim::UnitSpec& row = sim::spec_of(UnitKind::Power);
    EXPECT_STREQ(row.name, "power");
    EXPECT_EQ(row.cost, 30);
    EXPECT_EQ(row.build_ticks, 600) << "20 ticks a scrap: carried for the build-time slice";
    EXPECT_EQ(row.max_speed.raw, 0) << "a building never moves";
    EXPECT_EQ(row.range().raw, 0) << "and never fires";
    EXPECT_GT(row.hp, 0);
    EXPECT_TRUE(sim::has_footprint(UnitKind::Power));
    EXPECT_EQ(sim::footprint_of(UnitKind::Power), (sim::FootprintSize{4, 2}))
        << "provisional: a 4x2 rectangle, wider than deep, so rotation has something to turn";
    EXPECT_NE(sim::footprint_of(UnitKind::Power).width, sim::footprint_of(UnitKind::Power).height);
    EXPECT_EQ(sim::footprint_of(UnitKind::DeployedRecycler), (sim::FootprintSize{2, 2}))
        << "the recycler's 2x2 is untouched";
}

TEST(Power, AProducerCannotQueueABuildingAsAUnit) {
    // The row carries a cost and a build time, which is what "queueable" reads: a building must
    // not be buildable by a Factory's queue as if it were a vehicle.
    EXPECT_FALSE(sim::is_queueable(UnitKind::Power));
    sim::Scenario scenario{kSeed, {{UnitKind::Factory, Side::Player, 100, 100}}};
    scenario.start_ore = 100;
    scenario.store_capacity = 400;
    sim::Sim s = sim::Sim::create(scenario);
    s.enqueue(sim::Order{s.units().at(0).id, sim::orders::Build{UnitKind::Power, 0}});
    s.step();
    EXPECT_EQ(s.units().at(0).queue.count, 0U);
    EXPECT_EQ(s.ore(Side::Player), 100U);
}

// ------------------------------------------------------------------------------ placing

TEST(Power, APlaceOrderStandsTheBuildingAtOnceAndChargesThirty) {
    Yard y = make_yard(100);
    const Block b = y.block;
    y.sim.enqueue(place(y.constructor, b.x + 4, b.y + 5));
    y.sim.step();
    const sim::Unit* power = first_of(y.sim, UnitKind::Power);
    ASSERT_NE(power, nullptr) << "the building stands on the tick the order is applied";
    EXPECT_EQ(power->side, Side::Player);
    EXPECT_EQ(power->hp, sim::spec_of(UnitKind::Power).hp);
    EXPECT_EQ(y.sim.ore(Side::Player), 70U) << "30 scrap is gone from the bank";
    EXPECT_EQ(power->stance, sim::Stance::Idle) << "no half-built state";
}

TEST(Power, TheBuildingStandsOnExactlyItsFootprintAndNoUnitMayEnterIt) {
    Yard y = make_yard(100);
    const Block b = y.block;
    y.sim.enqueue(place(y.constructor, b.x + 4, b.y + 5));
    y.sim.step();
    for (int ty = b.y; ty < b.y + kBlockH; ++ty) {
        for (int tx = b.x; tx < b.x + kBlockW; ++tx) {
            const bool inside = tx >= b.x + 4 && tx < b.x + 8 && ty >= b.y + 5 && ty < b.y + 7;
            EXPECT_EQ(sim::building_blocks(y.sim.units(), tx, ty), inside) << tx << "," << ty;
        }
    }
    // A tank sent straight through it never sets a foot inside.
    y.sim.enqueue(sim::Order{
        y.tank, sim::orders::MoveTo{Fixed::from_int(b.x + 12), Fixed::from_int(b.y + 5)}});
    for (int tick = 0; tick < 600; ++tick) {
        y.sim.step();
        const sim::Unit* tank = y.sim.find(y.tank);
        ASSERT_NE(tank, nullptr);
        const int tx = tank->x.floor_int();
        const int ty = tank->y.floor_int();
        EXPECT_FALSE(tx >= b.x + 4 && tx < b.x + 8 && ty >= b.y + 5 && ty < b.y + 7)
            << "the tank entered the footprint at tick " << tick;
    }
}

TEST(Power, ARefusedPlacementChargesNothingAndPlacesNothing) {
    const Block b = open_block(kSeed);
    struct Case {
        const char* why;
        int tile_x;
        int tile_y;
    };
    const std::optional<Block> found = crater_tile(kSeed);
    ASSERT_TRUE(found.has_value());
    if (!found.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    const Block crater = *found;
    const std::vector<Case> cases{
        {"overlaps another building", b.x + 5, b.y + 6},  {"off the map (west)", -1, b.y + 5},
        {"off the map (south)", b.x + 4, sim::kMapTiles}, {"forbidden ground", crater.x, crater.y},
        {"a unit stands inside it", b.x + 1, b.y + 1},
    };
    for (const Case& c : cases) {
        Yard y = make_yard(100);
        y.sim.enqueue(place(y.constructor, b.x + 4, b.y + 5)); // the first one stands
        y.sim.step();
        ASSERT_EQ(count_of(y.sim, UnitKind::Power), 1U);
        ASSERT_EQ(y.sim.ore(Side::Player), 70U);
        y.sim.enqueue(place(y.constructor, c.tile_x, c.tile_y));
        y.sim.step();
        EXPECT_EQ(count_of(y.sim, UnitKind::Power), 1U) << c.why << ": nothing placed";
        EXPECT_EQ(y.sim.ore(Side::Player), 70U) << c.why << ": nothing charged";
    }
    // The ground is what refused the crater, not something else: the same anchor is refused by the
    // sim's own site rule.
    EXPECT_FALSE(
        sim::site_ok(kSeed, std::vector<sim::Unit>{}, sim::kPowerFootprint, crater.x, crater.y));
}

TEST(Power, ShortOfScrapAndTheWrongIssuerAreRefusedWithoutCharge) {
    Yard y = make_yard(29);
    y.sim.enqueue(place(y.constructor, y.block.x + 4, y.block.y + 5));
    y.sim.step();
    EXPECT_EQ(count_of(y.sim, UnitKind::Power), 0U) << "29 is short of 30";
    EXPECT_EQ(y.sim.ore(Side::Player), 29U);

    Yard z = make_yard(100);
    z.sim.enqueue(place(z.tank, z.block.x + 4, z.block.y + 5));
    z.sim.step();
    EXPECT_EQ(count_of(z.sim, UnitKind::Power), 0U) << "only a Constructor places a building";
    EXPECT_EQ(z.sim.ore(Side::Player), 100U);

    Yard w = make_yard(30);
    w.sim.enqueue(place(w.constructor, w.block.x + 4, w.block.y + 5));
    w.sim.step();
    EXPECT_EQ(count_of(w.sim, UnitKind::Power), 1U) << "exactly 30 is enough";
    EXPECT_EQ(w.sim.ore(Side::Player), 0U);
}

TEST(Power, ASecondPlacementInOneTickCannotOverspend) {
    Yard y = make_yard(30);
    y.sim.enqueue(place(y.constructor, y.block.x + 4, y.block.y + 5));
    y.sim.enqueue(place(y.constructor, y.block.x + 4, y.block.y + 9));
    y.sim.step();
    EXPECT_EQ(count_of(y.sim, UnitKind::Power), 1U);
    EXPECT_EQ(y.sim.ore(Side::Player), 0U);
}

// ------------------------------------------------------------------------------ rotation

TEST(Power, ARotatedPlacementSwapsWidthAndHeight) {
    Yard y = make_yard(100);
    const Block b = y.block;
    y.sim.enqueue(place(y.constructor, b.x + 4, b.y + 5, true));
    y.sim.step();
    const sim::Unit* power = first_of(y.sim, UnitKind::Power);
    ASSERT_NE(power, nullptr);
    EXPECT_TRUE(power->turned);
    EXPECT_EQ(sim::footprint_of(*power), (sim::FootprintSize{2, 4}));
    for (int ty = b.y; ty < b.y + kBlockH; ++ty) {
        for (int tx = b.x; tx < b.x + kBlockW; ++tx) {
            const bool inside = tx >= b.x + 4 && tx < b.x + 6 && ty >= b.y + 5 && ty < b.y + 9;
            EXPECT_EQ(sim::building_blocks(y.sim.units(), tx, ty), inside) << tx << "," << ty;
        }
    }
}

TEST(Power, ARotatedRectangleIsRefusedWhereTheUnrotatedOneFits) {
    // A stands at (4,5) covering x 4..7, y 5..6. B anchored at (6,3): unrotated it covers x 6..9,
    // y 3..4 -- clear of A; turned it covers x 6..7, y 3..6 -- on A's tiles.
    const Block b = open_block(kSeed);
    Yard y = make_yard(100);
    y.sim.enqueue(place(y.constructor, b.x + 4, b.y + 5));
    y.sim.step();
    ASSERT_EQ(y.sim.ore(Side::Player), 70U);
    const std::span<const sim::Unit> units{y.sim.units()};
    EXPECT_TRUE(sim::place_ok(kSeed, units, UnitKind::Power, false, b.x + 6, b.y + 3));
    EXPECT_FALSE(sim::place_ok(kSeed, units, UnitKind::Power, true, b.x + 6, b.y + 3));

    y.sim.enqueue(place(y.constructor, b.x + 6, b.y + 3, true));
    y.sim.step();
    EXPECT_EQ(count_of(y.sim, UnitKind::Power), 1U) << "the turned one is refused";
    EXPECT_EQ(y.sim.ore(Side::Player), 70U);
    y.sim.enqueue(place(y.constructor, b.x + 6, b.y + 3, false));
    y.sim.step();
    EXPECT_EQ(count_of(y.sim, UnitKind::Power), 2U) << "the unturned one fits";
    EXPECT_EQ(y.sim.ore(Side::Player), 40U);
}

// ------------------------------------------------------------------------------ determinism

TEST(Power, APlacementChangesTheStateHashAndReplaysToIt) {
    Yard a = make_yard(100);
    Yard c = make_yard(100);
    const std::uint64_t before = a.sim.state_hash();
    a.sim.enqueue(place(a.constructor, a.block.x + 4, a.block.y + 5, true));
    c.sim.enqueue(place(c.constructor, c.block.x + 4, c.block.y + 5, true));
    for (int i = 0; i < 5; ++i) {
        a.sim.step();
        c.sim.step();
    }
    EXPECT_NE(a.sim.state_hash(), before);
    EXPECT_EQ(a.sim.state_hash(), c.sim.state_hash()) << "same orders, same battle";
    Yard d = make_yard(100);
    d.sim.enqueue(place(d.constructor, d.block.x + 4, d.block.y + 5, false));
    for (int i = 0; i < 5; ++i) {
        d.sim.step();
    }
    EXPECT_NE(d.sim.state_hash(), a.sim.state_hash()) << "the turn is part of the state";
}

// ------------------------------------------------------------------------------ the panel

namespace game {
namespace {

using input::InputFrame;
using input::KeyEvent;

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

[[nodiscard]] Rig make_rig(std::uint16_t ore) {
    sim::Scenario scenario{
        kSeed,
        {{UnitKind::Tank, Side::Player, 60, 60}, {UnitKind::Constructor, Side::Player, 61, 60}}};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    Rig r{};
    r.sim = sim::Sim::create(scenario);
    r.driven = r.sim.units().at(0).id;
    r.constructor = r.sim.units().at(1).id;
    r.options.seed = kSeed;
    r.options.extent = Extent{1280, 720};
    r.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    r.options.viewer_unit = r.driven;
    r.options.aim = Aim{640.0F, 700.0F};
    r.row.update(r.sim, Side::Player, 0);
    r.options.row = &r.row;
    return r;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

void press(const Rig& r, Session& session, std::vector<sim::Order>& orders, std::int32_t code) {
    session.frame(key_frame(code), r.sim, r.options, orders);
}

void open_panel(const Rig& r, Session& session, std::vector<sim::Order>& orders) {
    for (int slot = 0; slot < 10; ++slot) {
        if (session.selection().size() == 1U
            && session.selection().units().front() == r.constructor) {
            break;
        }
        press(r, session, orders, input::kKeyF1 + slot);
    }
    ASSERT_EQ(session.selection().size(), 1U);
    press(r, session, orders, input::kKey1 + 2); // `3 Construct`
    ASSERT_TRUE(session.construct_armed());
}

/// The orders this frame addressed to the Constructor: the session also emits the driven tank's
/// own every frame, which is none of this slice's business.
[[nodiscard]] std::vector<sim::Order> constructor_orders(const Rig& r,
                                                         const std::vector<sim::Order>& orders) {
    std::vector<sim::Order> mine;
    for (const sim::Order& order : orders) {
        if (order.unit == r.constructor) {
            mine.push_back(order);
        }
    }
    return mine;
}

[[nodiscard]] const MenuRow* row_keyed(const std::vector<MenuRow>& rows, std::string_view key) {
    for (const MenuRow& row : rows) {
        if (row.key == key) {
            return &row;
        }
    }
    return nullptr;
}

[[nodiscard]] std::vector<MenuRow> rows_now(const Rig& r, const Session& session) {
    return app::menu_for(r.sim, session, r.options, session.attack_armed());
}

} // namespace

TEST(PowerPanel, OnlyThePowerRowCarriesAKindAndTheOtherEightStayNameRows) {
    const std::span<const BuildItem> items = construct_items();
    ASSERT_EQ(items.size(), 9U);
    ASSERT_TRUE(items[0].kind.has_value());
    EXPECT_EQ(items[0].kind.value_or(UnitKind::Tank), UnitKind::Power);
    EXPECT_EQ(items[0].cost, sim::spec_of(UnitKind::Power).cost)
        << "the price drawn is the price charged";
    for (std::size_t i = 1; i < items.size(); ++i) {
        EXPECT_FALSE(items[i].kind.has_value()) << items[i].label;
    }
    EXPECT_FALSE(items[1].kind.has_value())
        << "FACTORY is not buildable because a Factory kind exists";
}

TEST(PowerPanel, OnePressArmsPowerAndThePreviewCarriesItsCostAndFootprint) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    ASSERT_FALSE(session.placing().has_value()) << "opening the panel arms nothing";
    orders.clear();
    press(r, session, orders, input::kKey1);
    const std::optional<Placing> armed = session.placing();
    ASSERT_TRUE(armed.has_value()) << "1 on the panel ARMS POWER";
    if (!armed.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    EXPECT_EQ(armed->kind, UnitKind::Power);
    EXPECT_EQ(armed->cost, 30);
    EXPECT_EQ(armed->size, (sim::FootprintSize{4, 2}));
    EXPECT_TRUE(session.construct_armed()) << "the panel stays up while a building is armed";
    EXPECT_TRUE(constructor_orders(r, orders).empty()) << "arming issues no order";
    EXPECT_EQ(r.sim.ore(Side::Player), 400U) << "and charges nothing";

    // The preview: the footprint's grid at the aimed tile, green where the sim would accept it.
    const NavAim aim = nav_aim(r.sim, r.options);
    ASSERT_TRUE(aim.on_ground());
    const DeployPreview preview = placement_preview(r.sim, *armed, aim.tile_x, aim.tile_y);
    EXPECT_EQ(preview.site.width, 4);
    EXPECT_EQ(preview.site.height, 2);
    EXPECT_EQ(preview.site.x, aim.tile_x);
    EXPECT_EQ(preview.site.y, aim.tile_y);
    EXPECT_EQ(preview.valid,
              sim::place_ok(kSeed, r.sim.units(), UnitKind::Power, false, aim.tile_x, aim.tile_y));
}

TEST(PowerPanel, ThePreviewIsRedWhereTheSimWouldRefuse) {
    Rig r = make_rig(400);
    const std::optional<Block> found = crater_tile(kSeed);
    ASSERT_TRUE(found.has_value());
    if (!found.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    const Block crater = *found;
    const Placing armed{UnitKind::Power, false, 30, sim::footprint_of(UnitKind::Power)};
    EXPECT_FALSE(placement_preview(r.sim, armed, crater.x, crater.y).valid);
    EXPECT_FALSE(placement_preview(r.sim, armed, -1, 5).valid) << "off the map";
    const Block b = open_block(kSeed);
    EXPECT_TRUE(placement_preview(r.sim, armed, b.x + 4, b.y + 5).valid);
}

TEST(PowerPanel, SpacePlacesItOneOrderOnTheConstructorAndNothingIsChargedByTheGameLayer) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1);
    ASSERT_TRUE(session.placing().has_value());
    const NavAim aim = nav_aim(r.sim, r.options);
    ASSERT_TRUE(aim.on_ground());
    orders.clear();
    press(r, session, orders, input::kKeySpace);
    const std::vector<sim::Order> mine = constructor_orders(r, orders);
    ASSERT_EQ(mine.size(), 1U);
    EXPECT_EQ(mine.front().unit, r.constructor);
    const auto* order = std::get_if<sim::orders::Place>(&mine.front().payload);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->kind, UnitKind::Power);
    EXPECT_EQ(order->tile_x, aim.tile_x);
    EXPECT_EQ(order->tile_y, aim.tile_y);
    EXPECT_FALSE(order->turned);
    EXPECT_EQ(r.sim.ore(Side::Player), 400U) << "the sim charges, when the building stands";
    EXPECT_FALSE(session.placing().has_value()) << "a placed building disarms";

    // Applied: the scrap is gone and the building stands.
    r.sim.enqueue(mine.front());
    r.sim.step();
    EXPECT_EQ(r.sim.ore(Side::Player), 370U);
    EXPECT_EQ(count_of(r.sim, UnitKind::Power), 1U);
}

TEST(PowerPanel, ASpaceThatTheSimWouldRefuseIsRefusedHereOutLoudAndChargesNothing) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1);
    orders.clear();
    press(r, session, orders, input::kKeySpace); // the first stands
    ASSERT_EQ(constructor_orders(r, orders).size(), 1U);
    r.sim.enqueue(constructor_orders(r, orders).front());
    r.sim.step();
    ASSERT_EQ(count_of(r.sim, UnitKind::Power), 1U);
    // Arm again and place on the SAME ground: it overlaps the first.
    press(r, session, orders, input::kKey1);
    ASSERT_TRUE(session.placing().has_value());
    orders.clear();
    press(r, session, orders, input::kKeySpace);
    EXPECT_TRUE(constructor_orders(r, orders).empty()) << "a refused SPACE issues no order";
    EXPECT_EQ(session.message(), "There is no room to build there.");
    EXPECT_TRUE(session.placing().has_value()) << "and the building stays armed";
    EXPECT_EQ(r.sim.ore(Side::Player), 370U) << "nothing more is charged";
    EXPECT_EQ(count_of(r.sim, UnitKind::Power), 1U);
}

TEST(PowerPanel, RotateIsLiveOnlyWhileABuildingIsArmedAndSwapsTheAxes) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    const std::vector<MenuRow> idle_rows = rows_now(r, session);
    const MenuRow* idle = row_keyed(idle_rows, "~");
    ASSERT_NE(idle, nullptr);
    EXPECT_FALSE(idle->enabled) << "nothing armed: the row is as it was";
    EXPECT_EQ(idle->reason, MenuDim::DoesNotApply);
    press(r, session, orders, input::kKey1);
    const std::vector<MenuRow> live_rows = rows_now(r, session);
    const MenuRow* live = row_keyed(live_rows, "~");
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(live->enabled) << "armed: the row is live";
    EXPECT_EQ(live->verb, kVerbRotateBuildings);
    press(r, session, orders, input::kKeyGrave);
    const std::optional<Placing> turned = session.placing();
    ASSERT_TRUE(turned.has_value());
    EXPECT_TRUE(turned.value_or(Placing{}).turned);
    EXPECT_EQ(turned.value_or(Placing{}).size, (sim::FootprintSize{2, 4}))
        << "width and height swap";
    EXPECT_EQ(turned.value_or(Placing{}).cost, 30) << "the price does not";
    press(r, session, orders, input::kKeyGrave);
    const std::optional<Placing> back = session.placing();
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back.value_or(Placing{}).turned) << "a second quarter turn faces it back";
    EXPECT_EQ(back.value_or(Placing{}).size, (sim::FootprintSize{4, 2}));

    // A turned building is placed turned.
    press(r, session, orders, input::kKeyGrave);
    orders.clear();
    press(r, session, orders, input::kKeySpace);
    // `constructor_orders` returns the orders BY VALUE, so the vector has to be bound to a local:
    // taking the address of an element of the temporary left `order` dangling as soon as the full
    // expression ended, and ASan said so at the landing (heap-use-after-free, READ at line 567).
    const std::vector<sim::Order> placed = constructor_orders(r, orders);
    ASSERT_EQ(placed.size(), 1U);
    const auto* order = std::get_if<sim::orders::Place>(&placed.front().payload);
    ASSERT_NE(order, nullptr);
    EXPECT_TRUE(order->turned);
}

TEST(PowerPanel, ATurnWithNothingArmedIsWhatItWasAKeyThatClosesThePanel) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKeyGrave);
    EXPECT_FALSE(session.placing().has_value()) << "a turn with nothing armed turns nothing";
    EXPECT_FALSE(session.construct_armed()) << "as today: any other key closes the panel";
}

TEST(PowerPanel, TheOtherEightRowsStillRefuseAndChargeNothing) {
    // J4: with nothing standing, each is refused for what it is missing (not the J3 placeholder).
    const std::array<std::string, 8> want{"Needs power",
                                          "Needs power",
                                          "Needs a Factory",
                                          "Needs a Factory",
                                          "Needs power",
                                          "Needs an Armory",
                                          "Needs an Armory and a Service Bay",
                                          "Needs a Training Ctr."};
    for (int digit = 2; digit <= 9; ++digit) {
        Rig r = make_rig(400);
        Session session{r.driven};
        std::vector<sim::Order> orders;
        open_panel(r, session, orders);
        orders.clear();
        press(r, session, orders, input::kKey1 + digit - 1);
        EXPECT_EQ(session.message(), want.at(static_cast<std::size_t>(digit - 2))) << digit;
        EXPECT_FALSE(session.placing().has_value()) << digit;
        EXPECT_TRUE(constructor_orders(r, orders).empty()) << digit;
        EXPECT_EQ(r.sim.ore(Side::Player), 400U) << digit;
    }
}

TEST(PowerPanel, APoorBankKeepsPowerInertAndNamesTheShortfall) {
    Rig r = make_rig(29);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1);
    EXPECT_FALSE(session.placing().has_value()) << "an unaffordable press is inert";
    EXPECT_EQ(session.message(), "Add 1 scrap (have 29, costs 30)");
}

TEST(PowerPanel, TabDisarmsAndPlacingDoesNotDisturbTheOtherMenus) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1);
    ASSERT_TRUE(session.placing().has_value());
    press(r, session, orders, input::kKeyTab);
    EXPECT_FALSE(session.placing().has_value()) << "TAB closes the armed building";
    EXPECT_FALSE(session.construct_armed());
    EXPECT_FALSE(session.deploy_armed());
    EXPECT_FALSE(session.attack_armed());
    EXPECT_FALSE(session.repair_armed());
    EXPECT_FALSE(session.nav_list_armed());
    // The producers' lists are the sizes they were.
    EXPECT_EQ(build_items(Faction::Human, MenuSubject::Deployment).size(), 7U);
    EXPECT_EQ(build_items(Faction::Human, MenuSubject::Factory).size(), 8U);
    // A placement leaves the Constructor's own menu intact.
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1);
    press(r, session, orders, input::kKeySpace);
    EXPECT_FALSE(session.deploy_armed());
    EXPECT_FALSE(session.attack_armed());
    EXPECT_FALSE(session.nav_list_armed());
}

TEST(PowerPanel, ThePreviewDrawsTheRectanglesOwnGridInTheSimsColour) {
    const Block b = open_block(kSeed);
    const Placing wide{UnitKind::Power, false, 30, sim::FootprintSize{4, 2}};
    const Placing tall{UnitKind::Power, true, 30, sim::FootprintSize{2, 4}};
    EXPECT_EQ(deploy_preview_vertex_count(wide.size), deploy_preview_vertex_count(tall.size))
        << "the same grid, turned";
    const Rig r = make_rig(400);
    EXPECT_EQ(placement_preview(r.sim, tall, b.x + 4, b.y + 5).site.width, 2);
    EXPECT_EQ(placement_preview(r.sim, tall, b.x + 4, b.y + 5).site.height, 4);
}

} // namespace game
} // namespace protekt
