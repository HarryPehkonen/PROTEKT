// The battle log is what a human actually reads, so its grammar is frozen too. A log that drifts
// silently is worse than no log: it makes "the battle looked right" unfalsifiable.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

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
    EXPECT_EQ(at_tick_zero, 12) << "one status line per living unit at tick 0";
    EXPECT_EQ(at_tick_sixty, 12);
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

} // namespace
