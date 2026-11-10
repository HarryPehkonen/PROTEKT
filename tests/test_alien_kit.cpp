// AZ, the alien kit: the Guardian (the aliens' Turret) and the Hauler (their Tug) are no longer the
// human machines, and all three alien assets carry RIBS -- steps that meet at more than the crease
// angle (`kCreaseAngleDegrees`, 75), because smooth shading averages every closer pair and a
// segmentation built from gentler angles is blended into a dome. Everything here is drawing; the
// sim is never touched.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
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
constexpr float kRadiansToDegrees = 57.29578F;
constexpr Factions kHumanPlays{Faction::Human, Faction::Human};
constexpr Factions kAlienPlays{Faction::Alien, Faction::Alien};

/// One machine as the frame draws it (smoothing included), in its own frame: `x` across, `y` over
/// the ground it stands on, `z` along, faced exactly along +z so the walls of a box are on the
/// axes.
struct Body {
    std::vector<SceneVertex> vertices;
};

/// The first tile within reach whose ground is level, so the lean to the ground cannot shear a box
/// off the axes the flat-wall measure reads.
[[nodiscard]] sim::Sim level_world(UnitKind kind) {
    constexpr float kLevelSine = 0.02F;
    for (int ty = 40; ty < 90; ++ty) {
        for (int tx = 40; tx < 90; ++tx) {
            sim::Sim sim = sim::Sim::create(
                sim::Scenario{kSeed, std::vector<Spawn>{{kind, Side::Player, tx, ty}}});
            const sim::Unit& unit = sim.units().front();
            const math::Vec3 normal = ground_normal(kSeed, unit.x, unit.y);
            if (std::hypot(normal.at(0), normal.at(2)) < kLevelSine) {
                return sim;
            }
        }
    }
    ADD_FAILURE() << "no level ground within reach";
    return sim::Sim::create(sim::Scenario{kSeed, {}});
}

[[nodiscard]] Body body_of(UnitKind kind, const Factions& factions) {
    const sim::Sim with = level_world(kind);
    const sim::Sim empty = sim::Sim::create(sim::Scenario{kSeed, {}});
    const sim::Unit& unit = with.units().front();
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = ground_metres(kSeed, unit.x, unit.y);
    SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    options.factions = factions;
    options.facing = SceneOptions::Facing{unit.id, 0.0F, 0.0F, 0.0F};
    options.camera.x = x;
    options.camera.z = z - 8.0F;
    options.camera.y = ground + 5.0F;
    options.camera.pitch = -0.25F;
    const Scene base = build_scene(empty, options);
    const Scene scene = build_scene(with, options);
    const auto same = [](const SceneVertex& a, const SceneVertex& b) {
        return a.position == b.position && a.colour == b.colour && a.alpha == b.alpha && a.u == b.u
               && a.v == b.v && a.fog == b.fog;
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
    Body body;
    for (std::size_t i = head; i + tail < scene.vertices.size(); ++i) {
        SceneVertex v = scene.vertices.at(i);
        if (v.position.at(1) < ground + 0.05F) {
            continue; // the shadow lies 3 cm over the ground
        }
        v.position = {v.position.at(0) - x, v.position.at(1) - ground, v.position.at(2) - z};
        body.vertices.push_back(v);
    }
    return body;
}

struct Box {
    std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> high{std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};
    [[nodiscard]] float size(std::size_t axis) const { return high.at(axis) - low.at(axis); }
};

[[nodiscard]] Box box_of(const Body& body) {
    Box box;
    for (const SceneVertex& v : body.vertices) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            box.low.at(axis) = std::min(box.low.at(axis), v.position.at(axis));
            box.high.at(axis) = std::max(box.high.at(axis), v.position.at(axis));
        }
    }
    return box;
}

[[nodiscard]] float angle_between(const math::Vec3& a, const math::Vec3& b) {
    const float d = (a.at(0) * b.at(0)) + (a.at(1) * b.at(1)) + (a.at(2) * b.at(2));
    return std::acos(std::clamp(d, -1.0F, 1.0F)) * kRadiansToDegrees;
}

/// How many POSITIONS of the machine carry two lit normals more than `degrees` apart AFTER the
/// smooth pass: the places a hard edge survived it. A rib built at less than the crease angle is
/// averaged away and does not count; that is the whole point of the measure.
[[nodiscard]] std::size_t surviving_edges(const Body& body, float degrees) {
    std::map<std::array<float, 3>, std::vector<math::Vec3>> at;
    for (const SceneVertex& v : body.vertices) {
        const math::Vec3& n = v.normal;
        if (n.at(0) == 0.0F && n.at(1) == 0.0F && n.at(2) == 0.0F) {
            continue;
        }
        at[v.position].push_back(n);
    }
    std::size_t count = 0;
    for (const auto& [position, normals] : at) {
        bool hard = false;
        for (std::size_t i = 0; i < normals.size() && !hard; ++i) {
            for (std::size_t j = i + 1; j < normals.size() && !hard; ++j) {
                hard = angle_between(normals.at(i), normals.at(j)) > degrees;
            }
        }
        count += hard ? 1U : 0U;
    }
    return count;
}

/// A triangle this big (square metres) is a stretch of WALL; a rib's step is a strip a centimetre
/// or two deep, square to the sweep by design, and is far smaller.
constexpr float kWallTriangle = 0.02F;

/// The area, in square metres, of upright triangles whose plane is square to an axis of the
/// machine's frame: a flat wall, the face of a box. A loft's panels and steps are never square.
[[nodiscard]] float flat_wall_area(const Body& body) {
    float area = 0.0F;
    for (std::size_t i = 0; i + 2 < body.vertices.size(); i += 3) {
        const math::Vec3& a = body.vertices.at(i).position;
        const math::Vec3& b = body.vertices.at(i + 1).position;
        const math::Vec3& c = body.vertices.at(i + 2).position;
        const math::Vec3 ab{b.at(0) - a.at(0), b.at(1) - a.at(1), b.at(2) - a.at(2)};
        const math::Vec3 ac{c.at(0) - a.at(0), c.at(1) - a.at(1), c.at(2) - a.at(2)};
        const math::Vec3 n{(ab.at(1) * ac.at(2)) - (ab.at(2) * ac.at(1)),
                           (ab.at(2) * ac.at(0)) - (ab.at(0) * ac.at(2)),
                           (ab.at(0) * ac.at(1)) - (ab.at(1) * ac.at(0))};
        const float length = std::hypot(n.at(0), n.at(1), n.at(2));
        if (length < 1.0e-9F) {
            continue;
        }
        const float ny = std::fabs(n.at(1)) / length;
        const float side = std::max(std::fabs(n.at(0)), std::fabs(n.at(2))) / length;
        if (ny < 0.02F && side > 0.9995F && length * 0.5F > kWallTriangle) {
            area += length * 0.5F;
        }
    }
    return area;
}

[[nodiscard]] bool same_positions(const Body& a, const Body& b) {
    return a.vertices.size() == b.vertices.size()
           && std::equal(a.vertices.begin(), a.vertices.end(), b.vertices.begin(),
                         [](const auto& l, const auto& r) { return l.position == r.position; });
}

TEST(AlienKit, TheGuardianAndTheHaulerAreNotTheHumanMachines) {
    for (const UnitKind kind : {UnitKind::Turret, UnitKind::Tug}) {
        const Body human = body_of(kind, kHumanPlays);
        const Body alien = body_of(kind, kAlienPlays);
        ASSERT_FALSE(human.vertices.empty());
        ASSERT_FALSE(alien.vertices.empty());
        EXPECT_FALSE(same_positions(human, alien)) << static_cast<int>(kind);
        EXPECT_NE(human.vertices.size(), alien.vertices.size()) << static_cast<int>(kind);
    }
}

TEST(AlienKit, TheFactionPicksTheGuardianAndTheHaulerAndTheSideDoesNot) {
    for (const UnitKind kind : {UnitKind::Turret, UnitKind::Tug}) {
        EXPECT_TRUE(same_positions(body_of(kind, kAlienPlays),
                                   body_of(kind, Factions{Faction::Alien, Faction::Human})))
            << "the player's faction owns the player's machine: " << static_cast<int>(kind);
        EXPECT_TRUE(same_positions(body_of(kind, kHumanPlays),
                                   body_of(kind, Factions{Faction::Human, Faction::Alien})))
            << static_cast<int>(kind);
    }
}

TEST(AlienKit, NoHumanKindOtherThanTheThreeIsRestyled) {
    for (const UnitKind kind :
         {UnitKind::Tank, UnitKind::Walker, UnitKind::Constructor, UnitKind::Recycler}) {
        EXPECT_TRUE(same_positions(body_of(kind, kHumanPlays), body_of(kind, kAlienPlays)))
            << static_cast<int>(kind);
    }
}

// ----------------------------------------------------------------------------- the sizes stay

TEST(AlienKit, TheGuardianStandsWhereTheHumanTurretStands) {
    const Box human = box_of(body_of(UnitKind::Turret, kHumanPlays));
    const Box alien = box_of(body_of(UnitKind::Turret, kAlienPlays));
    EXPECT_NEAR(alien.high.at(1), human.high.at(1), 0.06F) << "as tall: the whip, 1.8 m";
    EXPECT_NEAR(alien.high.at(2), human.high.at(2), 0.03F) << "the gun reaches as far";
    EXPECT_GE(alien.size(0), human.size(0) - 0.12F) << "as broad";
    EXPECT_LE(alien.size(0), human.size(0) + 0.02F) << "and no broader";
    EXPECT_GE(alien.low.at(2), human.low.at(2) - 0.02F) << "no longer behind";
    EXPECT_LE(alien.low.at(2), human.low.at(2) + 0.2F) << "and no shorter behind";
}

TEST(AlienKit, TheHaulerStandsWhereTheHumanTugStands) {
    const Box human = box_of(body_of(UnitKind::Tug, kHumanPlays));
    const Box alien = box_of(body_of(UnitKind::Tug, kAlienPlays));
    EXPECT_NEAR(alien.size(2), human.size(2), 0.2F) << "as long, hitch to nose";
    EXPECT_LE(alien.size(0), human.size(0) + 0.02F) << "no broader";
    EXPECT_GE(alien.size(0), human.size(0) * 0.8F) << "and not slimmer";
    EXPECT_LE(alien.high.at(1), human.high.at(1) + 0.02F) << "no taller";
    EXPECT_GE(alien.high.at(1), human.high.at(1) - 0.2F) << "and not lower";
}

TEST(AlienKit, TheHarvesterIsAsBigAsItWas) {
    // Measured before AZ, on level ground: 5.45 m from the nose to the proboscis' tip, 2.20 m
    // across the splayed feet, 1.26 m to the top of the crest. Presentation: the machine may not
    // move.
    const Box alien = box_of(body_of(UnitKind::Scavenger, kAlienPlays));
    EXPECT_NEAR(alien.size(2), 5.45F, 0.1F);
    EXPECT_NEAR(alien.size(0), 2.2F, 0.1F);
    EXPECT_NEAR(alien.high.at(1), 1.353F, 0.06F);
}

// ----------------------------------------------------------------------------- the ribs

/// A margin over the crease, so a rib that only just clears it does not pass for one.
constexpr float kRibDegrees = kCreaseAngleDegrees + 10.0F;

TEST(AlienKit, TheHarvesterKeepsRibsThatTheSmoothPassCannotAverage) {
    const Body harvester = body_of(UnitKind::Scavenger, kAlienPlays);
    EXPECT_GE(surviving_edges(harvester, kRibDegrees), std::size_t{150})
        << "positions where two normals stay more than " << kRibDegrees << " degrees apart";
}

TEST(AlienKit, TheGuardianAndTheHaulerKeepRibsToo) {
    for (const UnitKind kind : {UnitKind::Turret, UnitKind::Tug}) {
        EXPECT_GE(surviving_edges(body_of(kind, kAlienPlays), kRibDegrees), std::size_t{150})
            << static_cast<int>(kind);
    }
}

TEST(AlienKit, TheRibsAreSteeperThanTheCreaseAngleAndNotJustBarelyMoreThanItsMargin) {
    // The steepest step on each: a plate's edge is nearly square to the surface it overlaps.
    for (const UnitKind kind : {UnitKind::Scavenger, UnitKind::Turret, UnitKind::Tug}) {
        EXPECT_GE(surviving_edges(body_of(kind, kAlienPlays), 100.0F), std::size_t{40})
            << static_cast<int>(kind);
    }
}

TEST(AlienKit, ThereIsNoFlatWallOnTheAlienMachines) {
    // The human turret and tug are boxes (anti-vacuity: the measure sees them); the alien ones are
    // swept rings, and a ring's panel is never square to the machine's axes.
    EXPECT_GT(flat_wall_area(body_of(UnitKind::Turret, kHumanPlays)), 0.5F);
    EXPECT_GT(flat_wall_area(body_of(UnitKind::Tug, kHumanPlays)), 0.5F);
    for (const UnitKind kind : {UnitKind::Scavenger, UnitKind::Turret, UnitKind::Tug}) {
        EXPECT_LT(flat_wall_area(body_of(kind, kAlienPlays)), 0.05F) << static_cast<int>(kind);
    }
}

TEST(AlienKit, TheAlienMachinesAreMassesWithManyFacesNotBoxes) {
    for (const UnitKind kind : {UnitKind::Scavenger, UnitKind::Turret, UnitKind::Tug}) {
        const Body body = body_of(kind, kAlienPlays);
        EXPECT_GE(body.vertices.size(), std::size_t{1000}) << static_cast<int>(kind);
    }
}

} // namespace
} // namespace protekt::game
