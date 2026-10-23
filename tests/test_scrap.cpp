#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::sim::Fixed;
using protekt::sim::Scrap;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 20261023;

/// A fight that ENDS: three tanks of one side against one of the other, on a patch of open ground
/// (tiles (57..59, 41) against (57, 43)), so the lone one is destroyed instead of the two sides
/// trading at range. The survivor is what makes this a wreck test rather than a battle test -- one
/// side loses a unit and the other does not.
Sim fight(std::uint64_t seed) {
    protekt::sim::Scenario scenario;
    scenario.seed = seed;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 57, 41});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 58, 41});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 59, 41});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 57, 43});
    return Sim::create(scenario);
}

/// The first unit of the sim that is not alive, or an invalid id if none is.
UnitId first_dead(const Sim& sim) {
    for (const auto& unit : sim.units()) {
        if (!unit.alive) {
            return unit.id;
        }
    }
    return UnitId{};
}

std::size_t dead_count(const Sim& sim) {
    std::size_t count = 0;
    for (const auto& unit : sim.units()) {
        if (!unit.alive) {
            ++count;
        }
    }
    return count;
}

/// Steps until the first unit dies, or gives up. Returns the tick it happened on (0 if it did not).
std::uint32_t step_until_a_death(Sim& sim, int limit) {
    for (int i = 0; i < limit; ++i) {
        sim.step();
        if (!first_dead(sim).valid()) {
            continue;
        }
        return sim.tick();
    }
    return 0;
}

/// What `scatter` itself would have put on the ground for a wreck: the reference the sim's own
/// litter is compared against, tile for tile.
std::vector<Scrap> reference(Sim& sim, UnitId id) {
    std::vector<Scrap> out;
    const protekt::sim::Unit* dead = sim.find(id);
    protekt::sim::scatter(out, sim.seed(), id, dead->x, dead->y);
    return out;
}

} // namespace

TEST(Scrap, TheGroundIsBareUntilSomethingDies) {
    // Litter is a consequence of a wreck, not a feature of the world: a battle with nothing to
    // fight has an empty ground, and so has one that is fighting but has not killed anything yet.
    protekt::sim::Scenario quiet;
    quiet.seed = kSeed;
    quiet.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 40, 40});
    Sim alone = Sim::create(quiet);
    for (int i = 0; i < 600; ++i) {
        alone.step();
    }
    EXPECT_TRUE(alone.scraps().empty()) << "a tank with nobody to shoot left litter";

    Sim battle = fight(kSeed);
    battle.step();
    EXPECT_TRUE(battle.scraps().empty()) << "the first tick cannot have killed anyone";
}

TEST(Scrap, AWreckLeavesThePiecesTheRulesSay) {
    Sim sim = fight(kSeed);
    ASSERT_NE(step_until_a_death(sim, 4000), 0U) << "the fight never resolved";
    ASSERT_EQ(dead_count(sim), 1U) << "this rig is sized for exactly one loss";
    ASSERT_EQ(sim.scraps().size(), static_cast<std::size_t>(protekt::sim::kScrapPerWreck));

    // And the pieces are exactly what the rule says they are, for that unit, in that place: the
    // same call `scatter` makes, compared piece for piece. A sim that scattered somewhere else --
    // or scattered for the wrong unit -- cannot match this.
    const std::vector<Scrap> want = reference(sim, first_dead(sim));
    ASSERT_EQ(want.size(), sim.scraps().size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(sim.scraps().at(i).x.raw, want.at(i).x.raw) << "piece " << i;
        EXPECT_EQ(sim.scraps().at(i).y.raw, want.at(i).y.raw) << "piece " << i;
    }
}

TEST(Scrap, EveryPieceLiesAtATileCentreWithinTheScatterRadius) {
    Sim sim = fight(kSeed);
    ASSERT_NE(step_until_a_death(sim, 4000), 0U);
    ASSERT_FALSE(sim.scraps().empty());
    const protekt::sim::Unit* wreck = sim.find(first_dead(sim));
    const int wreck_x = wreck->x.floor_int();
    const int wreck_y = wreck->y.floor_int();
    for (const Scrap& piece : sim.scraps()) {
        // On the grid the world is drawn on: a whole tile plus a half, never a fraction of one.
        EXPECT_EQ(piece.x.raw % Fixed::kOne, Fixed::kOne / 2) << "not a tile centre in x";
        EXPECT_EQ(piece.y.raw % Fixed::kOne, Fixed::kOne / 2) << "not a tile centre in y";
        EXPECT_LE(std::abs(piece.x.floor_int() - wreck_x), protekt::sim::kScrapScatterRadius);
        EXPECT_LE(std::abs(piece.y.floor_int() - wreck_y), protekt::sim::kScrapScatterRadius);
    }
}

TEST(Scrap, ThePiecesAreAFunctionOfTheSeedAndTheDeadUnitsId) {
    // The same wreck in the same world always lands the same way; a different seed or a different
    // unit moves it. This is the property that makes the litter replayable without an RNG.
    const Fixed x = Fixed::from_int(40);
    const Fixed y = Fixed::from_int(40);
    std::vector<Scrap> first;
    std::vector<Scrap> again;
    std::vector<Scrap> other_seed;
    std::vector<Scrap> other_id;
    protekt::sim::scatter(first, kSeed, UnitId{7}, x, y);
    protekt::sim::scatter(again, kSeed, UnitId{7}, x, y);
    protekt::sim::scatter(other_seed, kSeed + 1, UnitId{7}, x, y);
    protekt::sim::scatter(other_id, kSeed, UnitId{8}, x, y);
    EXPECT_EQ(first.size(), static_cast<std::size_t>(protekt::sim::kScrapPerWreck));
    const auto same_field = [](const std::vector<Scrap>& a, const std::vector<Scrap>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const Scrap& p, const Scrap& q) {
                              return p.x.raw == q.x.raw && p.y.raw == q.y.raw;
                          });
    };
    EXPECT_TRUE(same_field(first, again)) << "the same wreck scattered differently twice";
    EXPECT_FALSE(same_field(first, other_seed)) << "a new seed scattered the same pieces";
    EXPECT_FALSE(same_field(first, other_id)) << "a different unit scattered the same pieces";
}

TEST(Scrap, OneWreckDoesNotStackPiecesOnOneTile) {
    // Two pieces on one tile read as one piece, and a collector would find two things where the eye
    // sees one. Every wreck's pieces are on different tiles -- checked over many wrecks, because a
    // collision is rare and a single wreck could pass by luck.
    const Fixed x = Fixed::from_int(57);
    const Fixed y = Fixed::from_int(43);
    for (std::uint32_t id = 1; id <= 200; ++id) {
        std::vector<Scrap> pieces;
        protekt::sim::scatter(pieces, kSeed, UnitId{id}, x, y);
        ASSERT_EQ(pieces.size(), static_cast<std::size_t>(protekt::sim::kScrapPerWreck));
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            for (std::size_t j = i + 1; j < pieces.size(); ++j) {
                EXPECT_FALSE(pieces.at(i).x.raw == pieces.at(j).x.raw
                             && pieces.at(i).y.raw == pieces.at(j).y.raw)
                    << "wreck " << id << " stacked pieces " << i << " and " << j;
            }
        }
    }
}

TEST(Scrap, TheGroundStopsCollectingAtTheCap) {
    // The dial that keeps a war which never ends from growing state without bound. The cap is
    // reached by scattering many wrecks, which is what the sim does too.
    std::vector<Scrap> out;
    for (std::uint32_t id = 1; id <= 4000 && out.size() < protekt::sim::kMaxScrap; ++id) {
        protekt::sim::scatter(out, kSeed, UnitId{id}, Fixed::from_int(40), Fixed::from_int(40));
    }
    EXPECT_EQ(out.size(), protekt::sim::kMaxScrap);
    // And it stays there: another wreck adds nothing.
    protekt::sim::scatter(out, kSeed, UnitId{9999}, Fixed::from_int(40), Fixed::from_int(40));
    EXPECT_EQ(out.size(), protekt::sim::kMaxScrap);
}

TEST(Scrap, ADeadUnitScattersExactlyOnce) {
    // The wreck is made by the tick that killed the unit and never again: a battle that carries on
    // does not keep re-scattering the same corpse every tick.
    Sim sim = fight(kSeed);
    ASSERT_NE(step_until_a_death(sim, 4000), 0U);
    const std::size_t after_the_kill = sim.scraps().size();
    for (int i = 0; i < 400; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.scraps().size(), after_the_kill);
    EXPECT_EQ(after_the_kill,
              static_cast<std::size_t>(protekt::sim::kScrapPerWreck) * dead_count(sim))
        << "the litter must be one wreck's worth a death, no more and no less";
}

TEST(Scrap, TwoRunsOfTheSameBattleScatterTheSameLitter) {
    // Determinism, which is what lets the sim's own hash cover the litter: same seed, same battle,
    // same pieces in the same order -- and the same state hash.
    Sim first = fight(kSeed);
    Sim second = fight(kSeed);
    ASSERT_NE(step_until_a_death(first, 4000), 0U);
    ASSERT_NE(step_until_a_death(second, 4000), 0U);
    for (int i = 0; i < 200; ++i) {
        first.step();
        second.step();
    }
    ASSERT_FALSE(first.scraps().empty());
    ASSERT_EQ(first.scraps().size(), second.scraps().size());
    for (std::size_t i = 0; i < first.scraps().size(); ++i) {
        EXPECT_EQ(first.scraps().at(i).x.raw, second.scraps().at(i).x.raw) << "piece " << i;
        EXPECT_EQ(first.scraps().at(i).y.raw, second.scraps().at(i).y.raw) << "piece " << i;
    }
    EXPECT_EQ(first.state_hash(), second.state_hash());
}