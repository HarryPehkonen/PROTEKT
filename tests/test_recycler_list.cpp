// THE RECYCLER BUILDS ITS WHOLE LIST. Harri played and found the factory built everything and the
// recycler built two of its seven rows: the other five were names whose keys did nothing. Every
// number here is written out as a literal -- his own prices, in key order -- and none is read back
// from the table under test, so a table that drifted from his list cannot agree with itself.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Order;
using sim::Scenario;
using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

/// One row of Harri's recycler list: the key, the name, the price, and the kind it must queue.
struct Row {
    std::string_view key;
    std::string_view label;
    int cost;
    UnitKind kind;
};

constexpr std::array<Row, 7> kHarrisList{{{"1", "Scavenger", 20, UnitKind::Scavenger},
                                          {"2", "Scout", 50, UnitKind::Scout},
                                          {"3", "Turret", 40, UnitKind::Turret},
                                          {"4", "Tug", 45, UnitKind::Tug},
                                          {"5", "Constructor", 40, UnitKind::Constructor},
                                          {"6", "Service Truck", 50, UnitKind::ServiceTruck},
                                          {"7", "Service Pod", 2, UnitKind::ServicePod}}};

/// The kinds the list added: every one but the scavenger and the scout, which already existed.
constexpr std::array<UnitKind, 5> kAdded{UnitKind::Turret, UnitKind::Tug, UnitKind::Constructor,
                                         UnitKind::ServiceTruck, UnitKind::ServicePod};

[[nodiscard]] InputFrame key_frame(std::int32_t scancode, bool shift = false) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, shift});
    return frame;
}

} // namespace

TEST(RecyclerList, TheFourNewKindsAreAppendedAndNoEarlierValueMoved) {
    EXPECT_EQ(static_cast<int>(UnitKind::Tank), 0);
    EXPECT_EQ(static_cast<int>(UnitKind::MslScout), 14);
    EXPECT_EQ(static_cast<int>(UnitKind::DeployedScavenger), 15);
    EXPECT_EQ(static_cast<int>(UnitKind::DeployedTurret), 16);
    EXPECT_EQ(static_cast<int>(UnitKind::Tug), 17);
    EXPECT_EQ(static_cast<int>(UnitKind::Constructor), 18);
    EXPECT_EQ(static_cast<int>(UnitKind::ServiceTruck), 19);
    EXPECT_EQ(static_cast<int>(UnitKind::ServicePod), 20);
    EXPECT_EQ(sim::kUnitKindCount, 22); // moved 21 -> 22 with POWER (round 17, J3)
}

TEST(RecyclerList, EachKindIsQueueableAtHarrisPriceAndTwentyTicksAScrap) {
    struct Price {
        UnitKind kind;
        int cost;
        int ticks;
    };
    const std::array<Price, 5> prices{{{UnitKind::Turret, 40, 800},
                                       {UnitKind::Tug, 45, 900},
                                       {UnitKind::Constructor, 40, 800},
                                       {UnitKind::ServiceTruck, 50, 1000},
                                       {UnitKind::ServicePod, 2, 40}}};
    for (const Price& price : prices) {
        const sim::UnitSpec& spec = sim::spec_of(price.kind);
        EXPECT_EQ(spec.cost, price.cost) << spec.name;
        EXPECT_EQ(spec.build_ticks, price.ticks) << spec.name;
        EXPECT_TRUE(sim::is_queueable(price.kind)) << spec.name;
    }
}

TEST(RecyclerList, TheFourSupportKindsAreUnarmedMobileAndDrivable) {
    for (const UnitKind kind :
         {UnitKind::Tug, UnitKind::Constructor, UnitKind::ServiceTruck, UnitKind::ServicePod}) {
        const sim::UnitSpec& spec = sim::spec_of(kind);
        EXPECT_GT(spec.hp, 0) << spec.name;
        EXPECT_GT(spec.max_speed.raw, 0) << spec.name << " must be able to move";
        EXPECT_EQ(spec.range().raw, 0) << spec.name << " is unarmed";
        EXPECT_EQ(spec.damage, 0) << spec.name;
        EXPECT_TRUE(sim::is_drivable(kind)) << spec.name;
        EXPECT_FALSE(sim::is_tank(kind)) << spec.name;
        // The service truck owns a menu of its own (AC: `3 Repair`), and so does the Constructor
        // (round 17 J1: `3 Construct` opens the nine-row CONSTRUCT panel) -- RE-PINNED, not
        // relaxed: it was `Vehicle`, and now it is the Constructor's own subject, exactly as the
        // truck's is. The Tug and the Service Pod still share the vehicle's.
        const MenuSubject want = kind == UnitKind::ServiceTruck  ? MenuSubject::ServiceTruck
                                 : kind == UnitKind::Constructor ? MenuSubject::Constructor
                                                                 : MenuSubject::Vehicle;
        EXPECT_EQ(menu_subject_of_kind(kind), want) << spec.name;
    }
}

TEST(RecyclerList, TheHumanListIsHarrisSevenAndEveryRowNamesTheKindItQueues) {
    const std::span<const BuildItem> items = build_items(Faction::Human, MenuSubject::Deployment);
    ASSERT_EQ(items.size(), kHarrisList.size());
    for (std::size_t i = 0; i < kHarrisList.size(); ++i) {
        const BuildItem& got = *(items.begin() + static_cast<std::ptrdiff_t>(i));
        const Row& want = kHarrisList.at(i);
        EXPECT_EQ(got.key, want.key);
        EXPECT_EQ(got.label, want.label);
        EXPECT_EQ(got.cost, want.cost) << want.label;
        EXPECT_EQ(got.kind, std::optional<UnitKind>{want.kind})
            << want.label << " must name the kind it queues, not be a name-only row";
    }
}

namespace {

/// A deployed recycler beside a tank, with a bank that covers any one row: enough to see a key
/// queue something (a row the bank cannot cover is dimmed, and its press is refused).
struct Rig {
    sim::Sim sim;
    SceneOptions options;
    UnitId producer;
    Interaction player;
};

[[nodiscard]] Rig make_rig() {
    Scenario scenario{kSeed,
                      {{UnitKind::Tank, Side::Player, 40, 40},
                       {UnitKind::DeployedRecycler, Side::Player, 46, 41}}};
    scenario.store_capacity = 400;
    scenario.start_ore = 200;
    sim::Sim sim = sim::Sim::create(scenario);
    const UnitId tank = sim.units().at(0).id;
    const UnitId producer = sim.units().at(1).id;
    Rig rig{std::move(sim), SceneOptions{}, producer, Interaction{tank}};
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.player.handle(key_frame(input::kKey1), rig.sim, rig.options); // `1`: the recycler's menu
    return rig;
}

} // namespace

TEST(RecyclerList, PressingEachOfTheSevenKeysQueuesItsKindInAFreshRecycler) {
    for (std::size_t i = 0; i < kHarrisList.size(); ++i) {
        const Row& want = kHarrisList.at(i);
        Rig rig = make_rig();
        rig.player.handle(key_frame(input::kKey1 + static_cast<std::int32_t>(i)), rig.sim,
                          rig.options);
        const std::vector<Order> orders = rig.player.take_orders();
        ASSERT_EQ(orders.size(), 1U) << want.label << " ordered nothing";
        const auto* build = std::get_if<sim::orders::Build>(&orders.front().payload);
        ASSERT_NE(build, nullptr) << want.label;
        EXPECT_EQ(build->kind, want.kind) << want.label;
        rig.sim.enqueue(orders.front());
        rig.sim.step();
        const sim::Unit* producer = rig.sim.find(rig.producer);
        ASSERT_EQ(producer->queue.count, 1U) << want.label << " did not reach the queue";
        EXPECT_EQ(producer->queue.entries.front().kind, want.kind) << want.label;
    }
}

TEST(RecyclerList, EachAddedKindArrivesTakesAnFKeyAndDrives) {
    // A funded base: a deployed recycler, three haulers and wreckage to pay for the lot.
    Scenario scenario{kSeed,
                      {{UnitKind::Tank, Side::Player, 40, 40},
                       {UnitKind::DeployedRecycler, Side::Player, 52, 45},
                       {UnitKind::Scavenger, Side::Player, 48, 47},
                       {UnitKind::Scavenger, Side::Player, 48, 48},
                       {UnitKind::Scavenger, Side::Player, 48, 49},
                       {UnitKind::Factory, Side::Player, 50, 52}}};
    scenario.objectives = {};
    // The store opens at 40, so the 45 and 50 scrap rows could not be paid for until a scavenger
    // deployed; this test is about what arrives, not about the store, so the scenario widens it.
    scenario.store_capacity = 100;
    const sim::Fixed half = sim::Fixed::from_raw(sim::Fixed::kOne / 2);
    scenario.scrap.reserve(300);
    for (int i = 0; i < 300; ++i) {
        scenario.scrap.push_back(
            sim::Scrap{sim::fixed_add(sim::Fixed::from_int(42 + (i % 10)), half),
                       sim::fixed_add(sim::Fixed::from_int(30 + (i / 10)), half)});
    }
    sim::Sim sim = sim::Sim::create(scenario);
    const UnitId tank = sim.units().at(0).id;
    const UnitId producer = sim.units().at(1).id;
    const UnitId before = sim.units().back().id;

    const auto arrived = [&sim, before](UnitKind kind) -> const sim::Unit* {
        for (const sim::Unit& unit : sim.units()) {
            if (unit.kind == kind && unit.alive && unit.id.value > before.value) {
                return &unit;
            }
        }
        return nullptr;
    };
    // A queue holds one kind at a time, so each is built after the one before has arrived.
    for (const UnitKind kind : kAdded) {
        sim.enqueue(Order{producer, sim::orders::Build{kind, 0}});
        sim.step(); // the order reaches the queue on the next tick
        ASSERT_EQ(sim.find(producer)->queue.count, 1U) << sim::spec_of(kind).name;
        for (int tick = 0; tick < 12000 && sim.find(producer)->queue.count != 0; ++tick) {
            sim.step();
        }
        ASSERT_EQ(sim.find(producer)->queue.count, 0U) << sim::spec_of(kind).name << " never built";
    }

    RowState row;
    row.update(sim, Side::Player, 0);
    SceneOptions options;
    options.seed = kSeed;
    options.extent = kExtent;
    options.row = &row;
    for (const UnitKind kind : kAdded) {
        const sim::Unit* unit = arrived(kind);
        ASSERT_NE(unit, nullptr) << sim::spec_of(kind).name << " never arrived";
        EXPECT_EQ(unit->side, Side::Player);
        EXPECT_EQ(unit->hp, sim::spec_of(kind).hp);

        // It holds an F key, and that key selects exactly it.
        int key = 0;
        for (int n = 1; n <= kRowGroups; ++n) {
            const auto& slot = row.slots.at(group_slot(n)).units;
            if (slot.size() == 1 && slot.front() == unit->id) {
                key = n;
            }
        }
        ASSERT_NE(key, 0) << sim::spec_of(kind).name << " holds no F key";
        Interaction player{tank};
        player.handle(key_frame(input::kKeyF1 + key - 1), sim, options);
        const auto selected = player.selection().units();
        ASSERT_EQ(selected.size(), 1U) << sim::spec_of(kind).name;
        EXPECT_EQ(selected.front(), unit->id);
    }

    // And each one drives: ordered eight tiles away, it has covered a tile within eight seconds (a
    // mounted turret spends the first three unmounting).
    std::vector<std::pair<UnitId, std::int64_t>> starts;
    starts.reserve(kAdded.size());
    for (const UnitKind kind : kAdded) {
        const sim::Unit* unit = arrived(kind);
        starts.emplace_back(unit->id, unit->x.raw);
        sim.enqueue(Order{
            unit->id, sim::orders::MoveTo{sim::Fixed::from_int(unit->x.floor_int() + 8), unit->y}});
    }
    for (int tick = 0; tick < 240; ++tick) {
        sim.step();
    }
    for (const auto& [id, x_before] : starts) {
        EXPECT_GT(sim.find(id)->x.raw - x_before, sim::Fixed::kOne)
            << sim::spec_of(sim.find(id)->kind).name << " did not move a tile in eight seconds";
    }
}

} // namespace protekt::game
