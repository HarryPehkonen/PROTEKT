// The battle log is what a human actually reads, so its grammar is frozen too. A log that drifts
// silently is worse than no log: it makes "the battle looked right" unfalsifiable.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace {

using namespace protekt::sim;
namespace ai = protekt::ai;
namespace world = protekt::world;

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string token;
    while (in >> token) {
        out.push_back(token);
    }
    return out;
}

/// Recognises exactly the five frozen shapes. Anything else is a failure, which is the point: the
/// test fails on a log line nobody agreed to.
bool matches_grammar(const std::string& line) {
    // Token counts are the frozen grammar, not a guess:
    //   ordered  6:  tick <t> ordered unit <id> <verb>
    //   fired   10:  tick <t> fired unit <a> -> unit <s> hp <hp>
    //   killed   8:  tick <t> killed unit <s> by unit <a>
    //   status  15:  tick <t> status unit <id> <kind> side <side> pos <x> <y> hp <hp> stance <st>
    //   end      9:  end tick <t> seed <seed> units <n> hash <hash>
    const std::vector<std::string> t = tokens(line);
    const auto num = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    };
    const auto dec = [&num](const std::string& s) {
        const std::size_t dot = s.find('.');
        return dot != std::string::npos && num(s.substr(0, dot)) && num(s.substr(dot + 1))
               && s.substr(dot + 1).size() == 1;
    };
    if (t.size() == 6 && t[0] == "tick" && t[2] == "ordered" && t[3] == "unit") {
        return num(t[1]) && num(t[4]);
    }
    if (t.size() == 10 && t[0] == "tick" && t[2] == "fired" && t[3] == "unit" && t[5] == "->"
        && t[6] == "unit" && t[8] == "hp") {
        return num(t[1]) && num(t[4]) && num(t[7]) && num(t[9]);
    }
    if (t.size() == 8 && t[0] == "tick" && t[2] == "killed" && t[3] == "unit" && t[5] == "by"
        && t[6] == "unit") {
        return num(t[1]) && num(t[4]) && num(t[7]);
    }
    if (t.size() == 15 && t[0] == "tick" && t[2] == "status" && t[3] == "unit" && t[6] == "side"
        && t[8] == "pos" && t[11] == "hp" && t[13] == "stance") {
        return num(t[1]) && num(t[4]) && dec(t[9]) && dec(t[10]) && num(t[12]);
    }
    if (t.size() == 9 && t[0] == "end" && t[1] == "tick" && t[3] == "seed" && t[5] == "units"
        && t[7] == "hash") {
        return num(t[2]) && num(t[4]) && num(t[6]) && num(t[8]);
    }
    return false;
}

TEST(Observer, NamesAreTheFrozenLowercaseStrings) {
    EXPECT_STREQ(to_string(UnitKind::Tank), "tank");
    EXPECT_STREQ(to_string(UnitKind::Turret), "turret");
    EXPECT_STREQ(to_string(UnitKind::Factory), "factory");
    EXPECT_STREQ(to_string(Side::Player), "player");
    EXPECT_STREQ(to_string(Side::Enemy), "enemy");
    EXPECT_STREQ(to_string(Stance::Idle), "idle");
    EXPECT_STREQ(to_string(Stance::MoveTo), "move");
    EXPECT_STREQ(to_string(Stance::GuardAt), "guard");
    EXPECT_STREQ(to_string(Stance::Patrol), "patrol");
    EXPECT_STREQ(to_string(Stance::Attack), "attack");
    EXPECT_STREQ(to_string(Stance::Dead), "dead");
    EXPECT_STREQ(to_string(OrderKind::MoveTo), "move");
    EXPECT_STREQ(to_string(OrderKind::Stop), "stop");
}

TEST(Observer, EveryLineMatchesTheFrozenGrammarAndEndsWithTheHash) {
    const Scenario scenario = skirmish(42);
    const BattleLog log = run_battle(scenario, {}, 300, nullptr, nullptr);
    const std::string text = log.to_text(60);
    ASSERT_FALSE(text.empty());
    for (const std::string& line : lines_of(text)) {
        EXPECT_TRUE(matches_grammar(line)) << "unexpected log line: " << line;
    }
    const std::vector<std::string> all = lines_of(text);
    ASSERT_FALSE(all.empty());
    const std::vector<std::string> last = tokens(all.back());
    ASSERT_EQ(last.size(), 9U);
    EXPECT_EQ(last[0], "end");
    EXPECT_EQ(std::stoull(last[8]), log.final_hash) << "the summary must carry the real hash";
}

TEST(Observer, StatusLinesAppearEveryPeriodAndAlwaysAtTickZero) {
    const Scenario scenario = skirmish(42);
    const std::string text = run_battle(scenario, {}, 130, nullptr, nullptr).to_text(60);
    int at_tick_zero = 0;
    int at_tick_sixty = 0;
    for (const std::string& line : lines_of(text)) {
        const std::vector<std::string> t = tokens(line);
        if (t.size() == 15 && t[2] == "status") {
            if (t[1] == "0") {
                ++at_tick_zero;
            }
            if (t[1] == "60") {
                ++at_tick_sixty;
            }
        }
    }
    // Updated (RB1b): 20 units, the recycler of each side included.
    EXPECT_EQ(at_tick_zero, 20) << "one status line per living unit at tick 0";
    EXPECT_EQ(at_tick_sixty, 20);
}

TEST(Observer, PositionsPrintAsTilesWithOneDecimal) {
    const Scenario scenario = skirmish(42);
    const std::string text = run_battle(scenario, {}, 1, nullptr, nullptr).to_text(0);
    // With no commander and no script nothing moves, so the whole map is whole tiles: x.0.
    ASSERT_FALSE(lines_of(text).empty());
    bool saw_decimal = false;
    for (const std::string& line : lines_of(text)) {
        const std::vector<std::string> t = tokens(line);
        if (t.size() == 15 && t[2] == "status") {
            EXPECT_NE(t[9].find('.'), std::string::npos)
                << "position without a decimal point: " << line;
            EXPECT_EQ(t[10].substr(t[10].find('.') + 1).size(), 1U) << "exactly one decimal digit";
            saw_decimal = true;
        }
    }
    EXPECT_TRUE(saw_decimal);
}

TEST(Observer, TheLoggedHashIsTheStateTheBattleActuallyEndedIn) {
    const Scenario scenario = skirmish(42);
    const Tick ticks = 200;
    const BattleLog log = run_battle(scenario, {}, ticks, nullptr, nullptr);
    Sim sim = Sim::create(scenario);
    for (Tick t = 0; t < ticks; ++t) {
        sim.step();
    }
    EXPECT_EQ(log.final_hash, sim.state_hash());
    EXPECT_EQ(log.ticks, ticks);
}

TEST(Observer, EventsAreInNonDecreasingTickOrder) {
    const Scenario scenario = skirmish(42);
    ai::ScriptedCommander player(30);
    ai::ScriptedCommander enemy(30);
    const BattleLog log = run_battle(scenario, {}, 3000, &player, &enemy);
    ASSERT_FALSE(log.events.empty());
    for (std::size_t i = 1; i < log.events.size(); ++i) {
        EXPECT_LE(log.events[i - 1].tick, log.events[i].tick);
    }
}

TEST(Observer, AScriptedBattleActuallyFights) {
    const Scenario scenario = skirmish(42);
    ai::ScriptedCommander player(30);
    ai::ScriptedCommander enemy(30);
    const BattleLog log = run_battle(scenario, {}, 3000, &player, &enemy);
    const auto count = [&log](EventKind kind) {
        return std::count_if(log.events.begin(), log.events.end(),
                             [kind](const BattleEvent& e) { return e.kind == kind; });
    };
    EXPECT_GT(count(EventKind::Fired), 0) << "with both sides commanded, shots must be exchanged";
    EXPECT_GT(count(EventKind::Killed), 0) << "3000 ticks at 6 tiles a second is time enough";
    EXPECT_GT(count(EventKind::Ordered), 0) << "orders are part of the record";
}

TEST(Observer, TheSameBattleLogsIdentically) {
    const Scenario scenario = skirmish(42);
    ai::ScriptedCommander a(30);
    ai::ScriptedCommander b(30);
    EXPECT_EQ(run_battle(scenario, {}, 600, &a, &a).to_text(60),
              run_battle(scenario, {}, 600, &b, &b).to_text(60));
}

/// A reloaded tank whose gun has nothing to shoot, and an unarmed hostile factory just beyond its
/// weapon range. A gun with a target in range fires the moment it reloads, so a shot at nothing is
/// only possible when the hostile comes into range on the SAME tick: the tank is therefore ordered
/// to advance on it, and one tick of that move carries it inside the range.
struct Approach {
    Sim sim;
    UnitId tank{};
    UnitId hostile{};
};

bool in_range(const Sim& sim, UnitId a, UnitId b) {
    const Unit& from = *sim.find(a);
    const Unit& to = *sim.find(b);
    const std::int64_t dx = static_cast<std::int64_t>(from.x.raw) - to.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(from.y.raw) - to.y.raw;
    const std::int64_t range = profile(from.kind).weapon_range.raw;
    return dx * dx + dy * dy <= range * range;
}

Approach approach_field() {
    Scenario scenario{};
    scenario.seed = 42;
    const auto add = [&scenario](Side side, UnitKind kind, int tile_x, int tile_y) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = tile_x;
        spawn.tile_y = tile_y;
        scenario.spawns.push_back(spawn);
    };
    add(Side::Player, UnitKind::Tank, 100, 100);
    add(Side::Enemy, UnitKind::Factory, 112, 101);
    Approach out{Sim::create(scenario), {}, {}};
    out.tank = out.sim.units().at(0).id;
    out.hostile = out.sim.units().at(1).id;
    for (int t = 0; t < profile(UnitKind::Tank).cooldown_ticks; ++t) {
        out.sim.step();
    }
    return out;
}

/// Advance on the hostile, enqueue `trigger` on the same tick, step once, and return what the
/// observer called Fired. The premise is asserted: out of range before the step, in range after.
std::vector<BattleEvent> fired_while_closing_in(Approach& field, const Order& trigger) {
    EXPECT_FALSE(in_range(field.sim, field.tank, field.hostile));
    const Unit& hostile = *field.sim.find(field.hostile);
    field.sim.enqueue(Order{field.tank, orders::MoveTo{hostile.x, hostile.y}});
    field.sim.enqueue(trigger);
    const UnitSnapshot before = field.sim.units();
    field.sim.step();
    EXPECT_TRUE(in_range(field.sim, field.tank, field.hostile));
    std::vector<BattleEvent> fired;
    for (const BattleEvent& e : events_between(before, field.sim, field.sim.tick())) {
        if (e.kind == EventKind::Fired) {
            fired.push_back(e);
        }
    }
    return fired;
}

const orders::FireTowards kShotAtNothing{Fixed::from_int(-1), Fixed::zero()};

TEST(Observer, AShotAtNothingEmitsNoFiredEvent) {
    Approach field = approach_field();
    const std::vector<BattleEvent> fired
        = fired_while_closing_in(field, Order{field.tank, kShotAtNothing});
    ASSERT_EQ(field.sim.shots().size(), 1U) << "the shot at nothing really was made";
    ASSERT_EQ(field.sim.projectiles().size(), 1U);
    for (const BattleEvent& e : fired) {
        EXPECT_NE(e.subject, field.hostile) << "a shot at nothing was attributed to a unit";
    }
    EXPECT_TRUE(fired.empty());

    // The control: same field, same move, but the trigger names the hostile. THAT shot is reported.
    Approach control = approach_field();
    const std::vector<BattleEvent> named
        = fired_while_closing_in(control, Order{control.tank, orders::FocusFire{control.hostile}});
    ASSERT_EQ(named.size(), 1U);
    EXPECT_EQ(named[0].actor, control.tank);
    EXPECT_EQ(named[0].subject, control.hostile);
}

TEST(Observer, EveryShotTheSimReportsAppearsAsOneFiredEvent) {
    Approach field = approach_field();
    Sim& sim = field.sim;
    std::size_t valid_shots = 0;
    std::size_t shots_at_nothing = 0;
    std::vector<BattleEvent> fired;
    for (Tick t = 0; t < 200; ++t) {
        if (t == 0) {
            const Unit& hostile = *sim.find(field.hostile);
            sim.enqueue(Order{field.tank, orders::MoveTo{hostile.x, hostile.y}});
            sim.enqueue(Order{field.tank, kShotAtNothing});
        }
        const UnitSnapshot before = sim.units();
        sim.step();
        const std::size_t first = fired.size();
        for (const BattleEvent& e : events_between(before, sim, t)) {
            if (e.kind == EventKind::Fired) {
                fired.push_back(e);
            }
        }
        for (const Sim::Shot& shot : sim.shots()) {
            if (!shot.target.valid()) {
                ++shots_at_nothing;
                continue;
            }
            ++valid_shots;
            const auto begin = fired.begin() + static_cast<std::ptrdiff_t>(first);
            EXPECT_TRUE(std::any_of(begin, fired.end(),
                                    [&shot](const BattleEvent& e) {
                                        return e.actor == shot.shooter && e.subject == shot.target;
                                    }))
                << "a shot the sim made has no Fired event at tick " << t;
        }
    }
    EXPECT_GT(valid_shots, 1U) << "the tank keeps shooting once it is in range";
    EXPECT_EQ(shots_at_nothing, 1U);
    EXPECT_EQ(fired.size(), valid_shots);
}

TEST(Observer, TheHoverTankPrintsAsHovertank) {
    EXPECT_STREQ(to_string(UnitKind::HoverTank), "hovertank");
    EXPECT_STREQ(to_string(UnitKind::Tank), "tank");
}

TEST(Observer, TheFiredStreamForAScriptedBattleIsUnchanged) {
    // First recorded on the code BEFORE shots() existed, when Fired was still inferred from a
    // cooldown diff; RE-RECORDED for E9a, when the player gained a fourth unit (the hover tank,
    // id 4): every id after it moved up one and the battle itself changed. Columns: tick, actor,
    // subject, subject_hp.
    // RE-RECORDED again for R3a: the enemy gained its own hover tank (id 11), so the battle
    // changed.
    // RE-RECORDED for R3b2: each side gained a mortar and a rocket (ids 8, 9 and 17, 18), so every
    // enemy id moved up two; the ticks, the pairs and the hp are exactly R3a's -- the four support
    // weapons fire nothing in these 600 ticks.
    // RE-RECORDED for NAV1 (B2 -> B3): units now follow an A* route round craters instead of
    // steering at the goal, so they reach each other earlier and by different paths; the stream is
    // a different battle (first shot 438, 68 shots).
    // RE-RECORDED for F1 (B4 -> B5): the scripted commander now orders its mortar and rocket
    // (ids 8, 9 and 17, 18) to attack too, so they walk to range and fire; 69 shots.
    // RE-RECORDED for RB1b: each side gained a recycler (ids 10 and 20), appended after the rocket,
    // so every enemy id moved up one; the ticks, the pairs and the hp are exactly F1's -- the
    // unarmed recycler fires nothing and is shot at by nobody in these 600 ticks.
    struct Expected {
        Tick tick;
        std::uint32_t actor;
        std::uint32_t subject;
        std::uint16_t hp;
    };
    static const std::vector<Expected> kExpected = {
        {438, 2, 12, 100}, {438, 12, 2, 100}, {451, 11, 2, 100}, {453, 2, 11, 100},
        {453, 12, 2, 100}, {454, 14, 2, 100}, {459, 3, 12, 92},  {465, 1, 12, 92},
        {466, 11, 2, 92},  {468, 2, 11, 100}, {468, 12, 2, 84},  {469, 14, 2, 84},
        {471, 4, 12, 92},  {474, 3, 11, 100}, {480, 1, 11, 100}, {481, 11, 1, 100},
        {483, 2, 11, 100}, {483, 12, 1, 100}, {484, 14, 1, 100}, {486, 4, 11, 100},
        {486, 13, 1, 100}, {489, 3, 11, 100}, {495, 1, 11, 100}, {496, 11, 1, 100},
        {498, 2, 11, 100}, {498, 12, 1, 100}, {499, 14, 1, 100}, {501, 4, 11, 100},
        {501, 13, 1, 100}, {504, 3, 11, 100}, {510, 1, 11, 100}, {511, 11, 1, 68},
        {513, 2, 11, 100}, {513, 12, 1, 68},  {514, 14, 1, 68},  {515, 9, 12, 12},
        {516, 4, 11, 100}, {516, 13, 1, 68},  {519, 3, 11, 100}, {525, 1, 11, 92},
        {526, 11, 1, 36},  {528, 2, 11, 84},  {529, 14, 1, 36},  {531, 4, 11, 84},
        {531, 13, 1, 36},  {534, 3, 11, 76},  {540, 1, 11, 60},  {541, 11, 1, 4},
        {543, 2, 11, 28},  {544, 14, 1, 4},   {546, 4, 11, 28},  {546, 13, 1, 4},
        {549, 3, 11, 20},  {558, 2, 13, 100}, {559, 14, 4, 68},  {561, 4, 13, 100},
        {561, 13, 4, 68},  {564, 3, 13, 100}, {573, 2, 13, 100}, {574, 14, 2, 76},
        {576, 4, 13, 100}, {576, 13, 2, 76},  {579, 3, 13, 100}, {588, 2, 13, 76},
        {589, 14, 2, 76},  {590, 9, 13, 76},  {591, 4, 13, 76},  {591, 13, 2, 76},
        {594, 3, 13, 76},
    };
    const Scenario scenario = skirmish(42);
    ai::ScriptedCommander player(30);
    ai::ScriptedCommander enemy(30);
    const BattleLog log = run_battle(scenario, {}, 600, &player, &enemy);
    std::vector<Expected> got;
    for (const BattleEvent& e : log.events) {
        if (e.kind == EventKind::Fired) {
            got.push_back(Expected{e.tick, e.actor.value, e.subject.value, e.subject_hp});
        }
    }
    ASSERT_EQ(got.size(), std::size(kExpected));
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].tick, kExpected.at(i).tick) << "event " << i;
        EXPECT_EQ(got[i].actor, kExpected.at(i).actor) << "event " << i;
        EXPECT_EQ(got[i].subject, kExpected.at(i).subject) << "event " << i;
        EXPECT_EQ(got[i].hp, kExpected.at(i).hp) << "event " << i;
    }
}

TEST(ObserverPacking, KindIsSixBitsSideTwoStanceEight) {
    EXPECT_EQ(pack_unit_attrs(UnitKind::HoverTank, Side::Enemy, Stance::Follow),
              3U | (1U << 6) | (6U << 8));
    EXPECT_EQ(pack_unit_attrs(UnitKind::Tank, Side::Player, Stance::Idle), 0U);
}

TEST(ObserverPacking, EveryRealKindSideAndStanceRoundTrips) {
    for (int k = 0; k < kUnitKindCount; ++k) {
        for (const Side side : {Side::Player, Side::Enemy}) {
            for (int st = 0; st <= static_cast<int>(Stance::Follow); ++st) {
                const auto kind = static_cast<UnitKind>(k);
                const auto stance = static_cast<Stance>(st);
                const std::uint16_t packed = pack_unit_attrs(kind, side, stance);
                EXPECT_EQ(unpack_unit_kind(packed), kind);
                EXPECT_EQ(unpack_unit_side(packed), side);
                EXPECT_EQ(unpack_unit_stance(packed), stance);
            }
        }
    }
}

TEST(ObserverPacking, SyntheticKindsUpToSixtyThreeRoundTripWithoutBleedingIntoSide) {
    for (unsigned k = 0; k < 64; ++k) {
        const std::uint16_t packed
            = pack_unit_attrs(static_cast<UnitKind>(k), Side::Enemy, Stance::Dead);
        EXPECT_EQ(static_cast<unsigned>(unpack_unit_kind(packed)), k);
        EXPECT_EQ(unpack_unit_side(packed), Side::Enemy);
        EXPECT_EQ(unpack_unit_stance(packed), Stance::Dead);
    }
}

TEST(ObserverPacking, AKindPastTheTableIsPrintedUnknownNotCrashed) {
    EXPECT_STREQ(to_string(static_cast<UnitKind>(63)), "unknown");
}

/// A BUILD GROWS THE UNIT TABLE (C4). `events_between` returned NOTHING whenever the two sizes
/// disagreed, on the written assumption that the sim never resizes its table -- an assumption C4's
/// build made false the moment a factory started appending tanks, so every shot and kill of a build
/// tick was silently dropped out of the replay log and out of the sound. A table that GREW is the
/// same battle with one more unit on the END; only a table that SHRANK is a snapshot of something
/// else.
TEST(Observer, AGrownTableIsStillTheSameBattleAndItsEventsAreReported) {
    // A tank firing at a hostile factory, with an unarmed hauler parked far away and spawned LAST:
    // the last entry of the unit table is then a unit no shot or impact of the fight mentions,
    // which is what makes a shorter snapshot safe to diff -- dropping the last entry shortens the
    // table exactly the way a build lengthens it, without moving any other unit's index.
    protekt::sim::Scenario scenario;
    scenario.seed = 7;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 108, 100});
    scenario.spawns.push_back(Spawn{UnitKind::Factory, Side::Enemy, 112, 101});
    scenario.spawns.push_back(Spawn{UnitKind::Scavenger, Side::Player, 20, 20});
    Sim sim = Sim::create(scenario);
    const UnitId shooter = sim.units().at(0).id;
    const UnitId mark = sim.units().at(1).id;
    const UnitId parked = sim.units().back().id;

    bool checked = false;
    for (int i = 0; i < 900 && !checked; ++i) {
        const UnitSnapshot before = sim.units();
        // The tank is ordered onto the factory every tick: in range, the order arms a shot, which
        // is how this rig gets a tick with something to report.
        const Unit* target = sim.find(mark);
        ASSERT_NE(target, nullptr);
        sim.enqueue(Order{shooter, orders::FireTowards{target->x, target->y}});
        sim.step();
        const std::vector<BattleEvent> events = events_between(before, sim, sim.tick());
        if (events.empty()) {
            continue;
        }
        bool mentions_parked = false;
        for (const BattleEvent& e : events) {
            if (e.actor == parked || e.subject == parked) {
                mentions_parked = true;
            }
        }
        if (mentions_parked) {
            continue;
        }
        UnitSnapshot grown = before;
        grown.pop_back();
        // The same events, not none: the guard must not swallow a tick just because a build added a
        // unit to the end of the table on it.
        const std::vector<BattleEvent> after_the_build = events_between(grown, sim, sim.tick());
        EXPECT_FALSE(after_the_build.empty())
            << "a tick whose table grew (a build) must still report its events";
        EXPECT_EQ(after_the_build.size(), events.size());
        checked = true;
    }
    ASSERT_TRUE(checked) << "the rig must produce a tick with events that ignore the parked hauler";
}

} // namespace
