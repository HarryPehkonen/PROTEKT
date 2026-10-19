#include "protekt/ai/utility.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"

/// The composed behaviours, in a real battle. Unit tests describe what one unit decides; these are
/// the claims about a WAR -- and the one claim that started this file is the one no unit test could
/// have made: with a utility commander on each side, nothing ever happened at all.
namespace protekt::ai {
namespace {

using sim::BattleEvent;
using sim::BattleLog;
using sim::EventKind;

constexpr unsigned kSeed
    = 2; ///< a seed where the two commanders genuinely trade: see the note below
constexpr sim::Tick kTicks = 2400;

struct Fight {
    int player_shots{};
    int enemy_shots{};
    int player_lost{};
    int enemy_lost{};
    int first_shot{-1};
    std::uint64_t hash{};
};

[[nodiscard]] int side_of(const std::string& text, std::uint32_t id) {
    const std::string needle = "status unit " + std::to_string(id) + " ";
    int side = -1;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        const std::size_t marker = text.find(" side ", at);
        if (marker != std::string::npos) {
            side = text.compare(marker + 6, 6, "player") == 0 ? 0 : 1;
        }
    }
    return side;
}

/// Which side a unit belongs to is only readable from the log, because a killed unit stops
/// appearing in it -- so the last status line naming it is the evidence, exactly as the measurement
/// tool reads it. Both use the log rather than reaching into the simulation for a truth the
/// recording lacks.
[[nodiscard]] Fight fight(unsigned seed, sim::Tick ticks) {
    UtilityCommander player{UtilityTuning{}};
    UtilityCommander enemy{UtilityTuning{}};
    const BattleLog log = sim::run_battle(sim::skirmish(seed), {}, ticks, &player, &enemy);
    const std::string text = log.to_text(1000000);
    Fight result{};
    result.hash = log.final_hash;
    for (const BattleEvent& event : log.events) {
        if (event.kind == EventKind::Fired) {
            if (result.first_shot < 0) {
                result.first_shot = static_cast<int>(event.tick);
            }
            const int shooter = side_of(text, event.actor.value);
            if (shooter == 0) {
                ++result.player_shots;
            } else if (shooter == 1) {
                ++result.enemy_shots;
            }
        } else if (event.kind == EventKind::Killed) {
            const int victim = side_of(text, event.subject.value);
            if (victim == 0) {
                ++result.player_lost;
            } else if (victim == 1) {
                ++result.enemy_lost;
            }
        }
    }
    return result;
}

} // namespace

TEST(UtilityBattle, TwoUtilityCommandersActuallyFightEachOther) {
    // THE REGRESSION TEST for the degenerate equilibrium. Measured before the advance-to-contact
    // posture existed: with a utility commander on each side, ZERO shots were fired in 2400 ticks
    // on all ten seeds -- each side held at home and neither ever closed the distance out of its
    // 14-tile engagement range. No unit test could see it, because every unit was behaving exactly
    // as the unit tests say it should.
    const Fight result = fight(kSeed, kTicks);
    EXPECT_GE(result.first_shot, 0) << "nobody fired at all: the standoff is back";
    EXPECT_GT(result.player_shots, 0) << "the player's side never shot";
    EXPECT_GT(result.enemy_shots, 0) << "the enemy's side never shot";
    EXPECT_GT(result.player_lost + result.enemy_lost, 0) << "shots were fired and nothing died";
    EXPECT_LT(result.first_shot, static_cast<int>(kTicks) / 2)
        << "contact in the first half of the battle, not a last-moment accident";
}

TEST(UtilityBattle, ABattleBetweenTwoCommandersIsReproducible) {
    const Fight first = fight(kSeed, kTicks);
    const Fight second = fight(kSeed, kTicks);
    EXPECT_EQ(first.hash, second.hash);
    EXPECT_EQ(first.player_shots, second.player_shots);
    EXPECT_EQ(first.enemy_shots, second.enemy_shots);
    EXPECT_EQ(first.player_lost, second.player_lost);
    EXPECT_EQ(first.enemy_lost, second.enemy_lost);
    EXPECT_EQ(first.first_shot, second.first_shot);
}

TEST(UtilityBattle, TheCommandersFightWithTheOrdersTheSimulationAccepts) {
    // A commander can be perfectly decisive and still be ignored: every order it issues could be
    // refused by the simulation, and the failure would look exactly like a quiet battle. So this
    // asserts an effect that only happens if the orders landed -- enemies were hit by fire the
    // commander had to have set up, and both sides took losses.
    const Fight result = fight(kSeed, kTicks);
    EXPECT_GT(result.player_shots + result.enemy_shots, 4)
        << "four shots is one exchange; a battle is more than that";
    EXPECT_GT(result.player_lost, 0) << "one side lost nothing, which is not a fight";
    EXPECT_GT(result.enemy_lost, 0) << "one side lost nothing, which is not a fight";
}

TEST(UtilityBattle, ABattleWithAHillBetweenTheGunsStillEndsInAKill) {
    // THE REGRESSION TEST for the occlusion standoff. On these two seeds a rise stands between the
    // guns, and before a gun without a line moved instead of shooting, the two sides stood in range
    // of each other and traded no fire that could land: 120 seeds were run before this and after,
    // and 14 and 16 are the ones that had NO kill and now have one. They resolve because a unit
    // that cannot see its target steps aside (sideways on both, with a charge on 14 when neither
    // side gave a line) and fires when it can see.
    //
    // Not covered here, on purpose: seeds 42, 99, 1024 and 65535, which also end without a kill.
    // They are not occlusion -- they fire 45-72 bolts and 90% of them land -- they end with every
    // tank below the retreat threshold and withdrawing.
    for (const unsigned seed : {14U, 16U}) {
        const Fight result = fight(seed, kTicks);
        EXPECT_GT(result.player_lost + result.enemy_lost, 0)
            << "seed " << seed << ": the guns still cannot get a line to each other";
    }
}

} // namespace protekt::ai
