// The same kind, two looks: a Scavenger is drawn as the human hauler or the alien harvester by the
// FACTION its side plays (`SceneOptions::factions`), never by the side. All of it is drawing -- the
// sim is never touched.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "protekt/game/faction.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = 60;

/// The human Scavenger's drawn vertices (its shadow and its body) as they were BEFORE the harvester
/// existed, read off the build that preceded this slice: the count, and an FNV-1a hash over every
/// float of every vertex (position, colour, alpha, uv) by its bits. A human mesh that moves moves
/// this.
constexpr std::size_t kHumanScavengerVertices = 330;
/// The alien harvester's, read off its first build: the shadow (6) and its ten-cornered carapace,
/// proboscis, crest and six legs. Anti-vacuity's other half: a collapsed shape changes this.
constexpr std::size_t kAlienHarvesterVertices = 1362;
constexpr std::uint64_t kHumanScavengerHash = 15785359456797730901ULL;

/// The vertices a lone unit adds to a frame: what is left between the terrain and sky both frames
/// share before it, and the HUD both share after it. That is its shadow and its body.
[[nodiscard]] std::vector<SceneVertex> drawn(UnitKind kind, Side side, const Factions& factions) {
    const sim::Sim empty = sim::Sim::create(sim::Scenario{kSeed, {}});
    const sim::Sim with
        = sim::Sim::create(sim::Scenario{kSeed, std::vector<Spawn>{{kind, side, kTile, kTile}}});
    const sim::Unit& unit = with.units().front();
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    options.factions = factions;
    options.camera.x = x;
    options.camera.z = z - 8.0F;
    options.camera.y = ground_metres(kSeed, unit.x, unit.y) + 5.0F;
    options.camera.pitch = -0.25F;
    const Scene base = build_scene(empty, options);
    const Scene scene = build_scene(with, options);
    const auto same = [](const SceneVertex& a, const SceneVertex& b) {
        return a.position == b.position && a.colour == b.colour && a.alpha == b.alpha && a.u == b.u
               && a.v == b.v;
    };
    std::size_t head = 0;
    while (head < base.vertices.size() && head < scene.vertices.size()
           && same(base.vertices.at(head), scene.vertices.at(head))) {
        ++head;
    }
    std::size_t tail = 0;
    while (tail < base.vertices.size() - head && tail < scene.vertices.size() - head
           && same(base.vertices.at(base.vertices.size() - 1 - tail),
                   scene.vertices.at(scene.vertices.size() - 1 - tail))) {
        ++tail;
    }
    return {scene.vertices.begin() + static_cast<std::ptrdiff_t>(head),
            scene.vertices.end() - static_cast<std::ptrdiff_t>(tail)};
}

[[nodiscard]] std::uint64_t hash_of(const std::vector<SceneVertex>& vertices) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](float value) {
        hash = (hash ^ std::bit_cast<std::uint32_t>(value)) * 0x100000001B3ULL;
    };
    for (const SceneVertex& v : vertices) {
        for (const float p : v.position) {
            mix(p);
        }
        for (const float c : v.colour) {
            mix(c);
        }
        mix(v.alpha);
        mix(v.u);
        mix(v.v);
    }
    return hash;
}

[[nodiscard]] bool same_geometry(const std::vector<SceneVertex>& a,
                                 const std::vector<SceneVertex>& b) {
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(),
                         [](const auto& l, const auto& r) { return l.position == r.position; });
}

constexpr Factions kHumanPlays{Faction::Human, Faction::Human};
constexpr Factions kAlienPlays{Faction::Alien, Faction::Alien};

TEST(FactionLook, TheHumanScavengerIsExactlyWhatItWasBeforeTheHarvesterExisted) {
    const std::vector<SceneVertex> human = drawn(UnitKind::Scavenger, Side::Player, kHumanPlays);
    EXPECT_EQ(human.size(), kHumanScavengerVertices);
    EXPECT_EQ(hash_of(human), kHumanScavengerHash);
}

TEST(FactionLook, TheDefaultPlayerIsHumanAndTheDefaultScavengerIsTheHumanOne) {
    EXPECT_EQ(hash_of(drawn(UnitKind::Scavenger, Side::Player, Factions{})),
              hash_of(drawn(UnitKind::Scavenger, Side::Player, kHumanPlays)));
}

TEST(FactionLook, TheSameKindIsDrawnDifferentlyForAHumanAndAnAlien) {
    const std::vector<SceneVertex> human = drawn(UnitKind::Scavenger, Side::Player, kHumanPlays);
    const std::vector<SceneVertex> alien = drawn(UnitKind::Scavenger, Side::Player, kAlienPlays);
    ASSERT_FALSE(human.empty());
    ASSERT_FALSE(alien.empty());
    EXPECT_NE(alien.size(), human.size()) << "the alien silhouette is its own recipe";
    // Measured, not "unequal": how many of the first `n` vertices sit somewhere else.
    const std::size_t shared = std::min(human.size(), alien.size());
    std::size_t moved = 0;
    for (std::size_t i = 0; i < shared; ++i) {
        moved += human.at(i).position == alien.at(i).position ? 0U : 1U;
    }
    EXPECT_GT(moved * 10U, shared * 9U) << moved << " of " << shared << " vertices moved";
    // The harvester's own count: 6 shadow vertices and 1356 body, against the hauler's 330 in all.
    EXPECT_EQ(alien.size(), kAlienHarvesterVertices);
    EXPECT_EQ(human.size(), kHumanScavengerVertices);
}

TEST(FactionLook, TheAlienShapeIsRealGeometryNotAShrunkenFacet) {
    const std::vector<SceneVertex> alien = drawn(UnitKind::Scavenger, Side::Player, kAlienPlays);
    EXPECT_EQ(alien.size() % 3U, 0U) << "whole triangles";
    EXPECT_GE(alien.size(), std::size_t{300}) << "far more than one facet, or a box (36)";
    std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> high{std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};
    for (const SceneVertex& v : alien) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            low.at(axis) = std::min(low.at(axis), v.position.at(axis));
            high.at(axis) = std::max(high.at(axis), v.position.at(axis));
        }
    }
    // It must read at distance: as long as the human hauler (2.6 m), and as tall as its cab.
    EXPECT_GT(high.at(2) - low.at(2), 2.5F) << "long";
    EXPECT_GT(high.at(0) - low.at(0), 1.0F) << "broad";
    EXPECT_GT(high.at(1) - low.at(1), 0.8F) << "domed";
}

TEST(FactionLook, TheFactionPicksTheShapeAndTheSideDoesNot) {
    // Same side, faction swapped: the geometry follows the faction.
    EXPECT_FALSE(same_geometry(drawn(UnitKind::Scavenger, Side::Player, kHumanPlays),
                               drawn(UnitKind::Scavenger, Side::Player, kAlienPlays)));
    // Same faction, side swapped: the geometry is the same, only the colour is the side's.
    const std::vector<SceneVertex> alien_player
        = drawn(UnitKind::Scavenger, Side::Player, kAlienPlays);
    const std::vector<SceneVertex> alien_enemy
        = drawn(UnitKind::Scavenger, Side::Enemy, kAlienPlays);
    EXPECT_TRUE(same_geometry(alien_player, alien_enemy));
    EXPECT_NE(hash_of(alien_player), hash_of(alien_enemy)) << "the side still owns the colour";
    EXPECT_TRUE(same_geometry(drawn(UnitKind::Scavenger, Side::Player, kHumanPlays),
                              drawn(UnitKind::Scavenger, Side::Enemy, kHumanPlays)));
    // The default mapping read through the side: the enemy is the alien, the player the human...
    EXPECT_TRUE(same_geometry(drawn(UnitKind::Scavenger, Side::Enemy, Factions{}), alien_enemy));
    // ...and a story mission that swaps them swaps the shapes with them.
    const Factions swapped{Faction::Alien, Faction::Human};
    EXPECT_TRUE(same_geometry(drawn(UnitKind::Scavenger, Side::Player, swapped), alien_player));
    EXPECT_TRUE(same_geometry(drawn(UnitKind::Scavenger, Side::Enemy, swapped),
                              drawn(UnitKind::Scavenger, Side::Enemy, kHumanPlays)));
}

TEST(FactionLook, NoOtherKindIsRestyled) {
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Recycler, UnitKind::Walker}) {
        EXPECT_TRUE(same_geometry(drawn(kind, Side::Player, kHumanPlays),
                                  drawn(kind, Side::Player, kAlienPlays)))
            << static_cast<int>(kind);
    }
}

} // namespace
} // namespace protekt::game
