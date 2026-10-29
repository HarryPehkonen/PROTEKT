// ROW-START: the force the player starts with takes ONE GROUP KEY PER KIND (his ruling, applied to
// tick 0): three starting tanks share F1, every other starting vehicle kind takes the next key, and
// no building claims an F key.
#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "protekt/game/row.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

/// The row as the app has it at tick 0 over `data`.
[[nodiscard]] RowState start_row(const sim::Sim& sim) {
    RowState row;
    row.seed_from_start(sim, Side::Player, 0);
    return row;
}

[[nodiscard]] sim::Sim sim_of(const sim::ScenarioData& data) {
    return sim::Sim::create(sim::skirmish(kSeed, data));
}

[[nodiscard]] std::vector<UnitKind> kinds_in(const sim::Sim& sim, const RowSlot& slot) {
    std::vector<UnitKind> kinds;
    for (const UnitId id : slot.units) {
        kinds.push_back(sim.find(id)->kind);
    }
    return kinds;
}

[[nodiscard]] std::size_t visible_group_keys(const RowState& row) {
    std::size_t visible = 0;
    for (std::size_t index = kRowFirstGroup; index < kRowSlots; ++index) {
        visible += row.occupied(index) ? 1U : 0U;
    }
    return visible;
}

TEST(RowStart, TheThreeStartingTanksShareOneKey) {
    const sim::Sim sim = sim_of(sim::default_data());
    const RowState row = start_row(sim);

    EXPECT_EQ(kinds_in(sim, row.slots.at(group_slot(1))),
              (std::vector<UnitKind>{UnitKind::Tank, UnitKind::Tank, UnitKind::Tank}));
    int keys_with_a_tank = 0;
    for (int n = 1; n <= kRowGroups; ++n) {
        for (const UnitKind kind : kinds_in(sim, row.slots.at(group_slot(n)))) {
            keys_with_a_tank += kind == UnitKind::Tank ? 1 : 0;
        }
    }
    EXPECT_EQ(keys_with_a_tank, 3) << "no tank is held twice, and none is left out";
}

TEST(RowStart, EachOtherStartingVehicleKindHoldsItsOwnKey) {
    const sim::Sim sim = sim_of(sim::default_data());
    const RowState row = start_row(sim);

    EXPECT_EQ(kinds_in(sim, row.slots.at(group_slot(2))),
              (std::vector<UnitKind>{UnitKind::HoverTank}));
    EXPECT_EQ(kinds_in(sim, row.slots.at(group_slot(3))),
              (std::vector<UnitKind>{UnitKind::MortarBike}));
    EXPECT_EQ(kinds_in(sim, row.slots.at(group_slot(4))),
              (std::vector<UnitKind>{UnitKind::RocketTank}));
}

TEST(RowStart, NoBuildingSitsInAGroupKey) {
    const sim::Sim sim = sim_of(sim::default_data());
    const RowState row = start_row(sim);

    for (int n = 1; n <= kRowGroups; ++n) {
        for (const UnitKind kind : kinds_in(sim, row.slots.at(group_slot(n)))) {
            EXPECT_NE(kind, UnitKind::Factory) << "F" << n;
            EXPECT_NE(kind, UnitKind::Turret) << "F" << n;
            EXPECT_NE(kind, UnitKind::DeployedRecycler) << "F" << n;
        }
    }
}

TEST(RowStart, TheVisibleFKeysAreTheStartingVehicleKinds) {
    const sim::Sim sim = sim_of(sim::default_data());
    const RowState row = start_row(sim);

    // Tank, hover tank, mortar bike, rocket tank, then the scavenger and the undeployed recycler
    // (vehicles that move, the recycler until it deploys), one key each.
    EXPECT_EQ(visible_group_keys(row), 6U);
    EXPECT_EQ(kinds_in(sim, row.slots.at(group_slot(5))),
              (std::vector<UnitKind>{UnitKind::Scavenger}));
    EXPECT_EQ(kinds_in(sim, row.slots.at(group_slot(6))),
              (std::vector<UnitKind>{UnitKind::Recycler}));
}

TEST(RowStart, OneScenarioGroupOfThreeSharesOneKey) {
    // A scenario file's `count = 3`: the three units are one kind, so one key.
    sim::ScenarioData data;
    data.groups = {{UnitKind::Tank, 3, 4, 4}, {UnitKind::Recycler, 1, -3, -3}};
    const sim::Sim sim = sim_of(data);
    const RowState row = start_row(sim);

    EXPECT_EQ(row.slots.at(group_slot(1)).units.size(), 3U);
    EXPECT_EQ(visible_group_keys(row), 2U) << "the tanks, and the recycler";
}

TEST(RowStart, AfterTickZeroTheRowRulesAreTheOldOnes) {
    const sim::Sim sim = sim_of(sim::default_data());
    RowState row = start_row(sim);
    const RowState seeded = row;

    row.update(sim, Side::Player, 0);

    EXPECT_EQ(row.slots, seeded.slots) << "an update changes nothing about a settled row";
}

} // namespace
} // namespace protekt::game
