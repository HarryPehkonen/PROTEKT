#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/timings.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

/// N9: the guns choose a target, and prove a line to it, on a BEAT -- not every tick.
///
/// Measured (N7, docs/FRAMECOST.md): the target search and its line of sight were the whole cost
/// of a battle, 93% of `Sim::step` at the heavy size, because every armed unit whose gun was ready
/// searched every enemy every tick -- and the ones whose shot a friend blocked never got to fire,
/// so they were ready again the next tick and searched again (257 refusals a tick against 0.79
/// shots). Now a unit searches once per `kBeat` ticks (the AI's own 15-tick beat, phased by unit
/// id) and KEEPS what it found between beats.
///
/// The searches are COUNTED through `StepTimings::line_calls`: every candidate a search examines
/// in range makes one line-of-sight call, so a tick with no call is a tick with no search.
namespace protekt::sim {
namespace {

constexpr int kBeat = 15;

struct RowAt {
    int x{};
    int y{};
};

/// A level, open row (the friendly-fire tests' own helper, for the same reason: flat bolts).
[[nodiscard]] RowAt level_row(int length) {
    const auto height = [](int x, int y) {
        return static_cast<int>(ground_height(42, Fixed::from_int(x), Fixed::from_int(y)).raw
                                / Fixed::kOne);
    };
    for (int y = 40; y < kMapTiles - 40; ++y) {
        for (int x = 40; x + length < kMapTiles - 40; ++x) {
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                level = std::abs(height(x + i, y) - height(x, y)) <= 4;
            }
            for (int ty = y - 10; ty <= y + 12 && level; ++ty) {
                for (int tx = x - 1; tx <= x + length + 5 && level; ++tx) {
                    level = tile_passable(42, tx, ty);
                }
            }
            if (level) {
                return RowAt{x, y};
            }
        }
    }
    return RowAt{-1, -1};
}

struct Added {
    Side side;
    UnitKind kind;
    int dx;
    int dy;
};

struct Field {
    Sim sim;
    std::vector<UnitId> ids; ///< in the order given
};

[[nodiscard]] Field field(const std::vector<Added>& units) {
    Scenario scenario{};
    scenario.seed = 42;
    const RowAt row = level_row(14);
    for (const Added& add : units) {
        Spawn spawn{};
        spawn.side = add.side;
        spawn.kind = add.kind;
        spawn.tile_x = row.x + add.dx;
        spawn.tile_y = row.y + add.dy;
        scenario.spawns.push_back(spawn);
    }
    Field out{};
    out.sim = Sim::create(scenario);
    for (const Unit& unit : out.sim.units()) {
        out.ids.push_back(unit.id);
    }
    return out;
}

/// What one step did: the tick it ran on, how many line-of-sight calls the guns made, and the
/// shots.
struct Stepped {
    std::uint32_t tick{};
    std::size_t lines{};
    std::vector<Sim::Shot> shots;
};

[[nodiscard]] Stepped step_once(Sim& sim) {
    Stepped out{};
    out.tick = sim.tick();
    StepTimings timings{};
    sim.step(&timings);
    out.lines = timings.line_calls;
    out.shots = sim.shots();
    return out;
}

[[nodiscard]] std::vector<Stepped> run(Sim& sim, int ticks) {
    std::vector<Stepped> out;
    out.reserve(static_cast<std::size_t>(ticks));
    for (int i = 0; i < ticks; ++i) {
        out.push_back(step_once(sim));
    }
    return out;
}

[[nodiscard]] std::vector<std::uint32_t> search_ticks(const std::vector<Stepped>& steps) {
    std::vector<std::uint32_t> ticks;
    for (const Stepped& step : steps) {
        if (step.lines > 0) {
            ticks.push_back(step.tick);
        }
    }
    return ticks;
}

/// A tank whose every shot a friendly factory five tiles east would take, and a hostile beyond the
/// factory in range: the gun finds a candidate, has a line, and the friend probe refuses -- the
/// 99%-of-the-time state N7 measured. The tank never fires.
[[nodiscard]] Field blocked_lane() {
    return field({{Side::Player, UnitKind::Tank, 0, 0},
                  {Side::Player, UnitKind::Factory, 1, -4},
                  {Side::Enemy, UnitKind::Recycler, 11, 0}});
}

/// A scout (cooldown 5, so most of its shots fall BETWEEN beats) and an unarmed hostile four tiles
/// south, in the open.
[[nodiscard]] Field open_shot() {
    return field({{Side::Player, UnitKind::Scout, 0, 0}, {Side::Enemy, UnitKind::Recycler, 0, 4}});
}

TEST(FireBeat, ABlockedLaneIsSearchedOnTheBeatAndNotBetween) {
    Field f = blocked_lane();
    const std::vector<Stepped> steps = run(f.sim, 90);
    for (const Stepped& step : steps) {
        ASSERT_TRUE(step.shots.empty()) << "the fixture must keep the tank from firing";
    }
    const std::vector<std::uint32_t> ticks = search_ticks(steps);
    ASSERT_GE(ticks.size(), 2U) << "a gun with no target still searches, on its beat";
    EXPECT_LE(ticks.size(), 7U) << "90 ticks hold six beats; the old path searched all 90";
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        EXPECT_EQ(ticks.at(i) - ticks.at(i - 1), static_cast<std::uint32_t>(kBeat));
    }
}

TEST(FireBeat, AUnitWithNoTargetStillSearchesOnItsBeatAndFindsOneWithinOne) {
    // A scout 13 tiles from an unarmed hostile (its reach is 10) is told to close to 9: out of
    // range it has no target; the tick it is in range it must find one within one beat.
    Field f
        = field({{Side::Player, UnitKind::Scout, 0, 0}, {Side::Enemy, UnitKind::Recycler, 13, 0}});
    const Unit* scout = f.sim.find(f.ids.at(0));
    const Unit* hostile = f.sim.find(f.ids.at(1));
    f.sim.enqueue(Order{f.ids.at(0),
                        orders::MoveTo{Fixed::from_int(scout->x.raw / Fixed::kOne + 4), scout->y}});
    const std::int64_t reach = profile(UnitKind::Scout).weapon_range.raw;
    int entered = -1;
    int first_shot = -1;
    std::vector<Stepped> steps;
    for (int tick = 0; tick < 120 && first_shot < 0; ++tick) {
        steps.push_back(step_once(f.sim));
        const std::int64_t dx = scout->x.raw - hostile->x.raw;
        const std::int64_t dy = scout->y.raw - hostile->y.raw;
        if (entered < 0 && dx * dx + dy * dy <= reach * reach) {
            entered = tick;
        }
        if (!steps.back().shots.empty()) {
            first_shot = tick;
        }
    }
    ASSERT_GE(entered, 0);
    ASSERT_GE(first_shot, 0) << "a unit with no target must still find one";
    EXPECT_LE(first_shot - entered, kBeat + 1);
    const std::vector<std::uint32_t> ticks = search_ticks(steps);
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        EXPECT_EQ(ticks.at(i) - ticks.at(i - 1), static_cast<std::uint32_t>(kBeat));
    }
    // ...and found it no sooner than a beat would: on the old path the shot came the tick it was
    // in range.
    EXPECT_GT(first_shot - entered, 0)
        << "entered range on a beat tick by chance; move the fixture";
}

TEST(FireBeat, AKeptTargetIsShotAtBetweenBeatsWithNoSearch) {
    Field f = open_shot();
    const std::vector<Stepped> steps = run(f.sim, 120);
    int unsearched_shots = 0;
    int shots = 0;
    for (const Stepped& step : steps) {
        shots += static_cast<int>(step.shots.size());
        if (!step.shots.empty() && step.lines == 0) {
            ++unsearched_shots;
        }
    }
    EXPECT_GE(shots, 15) << "the scout fires every five ticks";
    EXPECT_GE(unsearched_shots, 10) << "most of those shots keep the target and search nothing";
    for (const Stepped& step : steps) {
        for (const Sim::Shot& shot : step.shots) {
            EXPECT_EQ(shot.target, f.ids.at(1));
        }
    }
}

TEST(FireBeat, TheLineOfSightIsNotRerunBetweenBeatsForAnUnchangedTarget) {
    Field f = open_shot();
    const std::vector<Stepped> steps = run(f.sim, 120);
    const std::vector<std::uint32_t> ticks = search_ticks(steps);
    ASSERT_GE(ticks.size(), 2U);
    EXPECT_LE(ticks.size(), 9U) << "120 ticks hold eight beats; the old path made one call a shot";
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        EXPECT_EQ(ticks.at(i) - ticks.at(i - 1), static_cast<std::uint32_t>(kBeat));
    }
}

TEST(FireBeat, ADeadTargetIsDroppedAndTheNextTakenWithinABeat) {
    // Two unarmed hostiles in a column south of the scout: the near one dies first.
    Field f = field({{Side::Player, UnitKind::Scout, 0, 0},
                     {Side::Enemy, UnitKind::Recycler, 0, 4},
                     {Side::Enemy, UnitKind::Recycler, 0, 8}});
    const UnitId near = f.ids.at(1);
    const UnitId far = f.ids.at(2);
    int died = -1;
    int next_shot = -1;
    for (int tick = 0; tick < 1500 && next_shot < 0; ++tick) {
        const Stepped step = step_once(f.sim);
        if (died < 0 && !f.sim.find(near)->alive) {
            died = tick;
        }
        for (const Sim::Shot& shot : step.shots) {
            if (died >= 0 && tick >= died) {
                EXPECT_NE(shot.target, near) << "a dead target is never shot again";
                if (shot.target == far && next_shot < 0) {
                    next_shot = tick;
                }
            }
        }
    }
    ASSERT_GE(died, 0) << "the near hostile must die for this to say anything";
    ASSERT_GE(next_shot, 0) << "the far one must be taken up";
    EXPECT_LE(next_shot - died, kBeat + 5) << "a beat to find it, a reload to fire";
}

} // namespace
} // namespace protekt::sim
