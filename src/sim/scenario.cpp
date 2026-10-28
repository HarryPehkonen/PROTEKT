#include "protekt/sim/scenario.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <vector>

#include "protekt/rng.hpp"
#include "protekt/world/world.hpp"

namespace protekt::sim {

namespace {

constexpr std::uint64_t kScenarioStream = 3; ///< world = 0, sim = 1, ai = 2 are taken elsewhere
/// Player base centres start in [kEdge, kEdge + kJitter), enemy ones in [kEnemyEdge, kEnemyEdge +
/// kJitter): the high quadrant, but close enough that a tank (which steers in a straight line, with
/// no pathfinding) has a fair chance of crossing the ground between the two before a crater stops
/// it.
constexpr int kEdge = 24;
constexpr int kEnemyEdge = 144;
constexpr int kJitter = 24;
constexpr int kBaseAttempts = 32; ///< enemy base candidates tried per seed

bool passable(std::uint64_t seed, int x, int y) {
    return x >= 0 && y >= 0 && x < kMapTiles && y < kMapTiles
           && terrain_speed_factor(world::sample(seed, x, y).terrain).raw != 0;
}

/// Whether a spawn already placed stands on the tile: a unit on its own tile, a building (its
/// spawn tile is its footprint's north-west corner, as `Sim::create` reads it) on any tile of its
/// footprint -- so nothing is placed inside a building.
bool taken(const std::vector<Spawn>& spawns, int x, int y) {
    return std::any_of(spawns.begin(), spawns.end(), [x, y](const Spawn& s) {
        return has_footprint(s.kind) ? footprint_at(s.tile_x, s.tile_y).covers(x, y)
                                     : s.tile_x == x && s.tile_y == y;
    });
}

/// The wanted tile if free and passable, otherwise the first one found on growing square rings in
/// row-major order. Falls back to the clamped wish if the whole map is somehow blocked. A building
/// needs its whole footprint free and passable, so it never covers a unit placed before it either.
Spawn place(std::uint64_t seed, const std::vector<Spawn>& spawns, UnitKind kind, Side side, int x,
            int y) {
    x = std::clamp(x, 0, kMapTiles - 1);
    y = std::clamp(y, 0, kMapTiles - 1);
    const auto free
        = [&](int tx, int ty) { return passable(seed, tx, ty) && !taken(spawns, tx, ty); };
    const auto ok = [&](int tx, int ty) {
        if (!has_footprint(kind)) {
            return free(tx, ty);
        }
        if (tx < 0 || ty < 0 || tx >= kMapTiles || ty >= kMapTiles) {
            return false;
        }
        const Footprint site = footprint_at(tx, ty);
        for (int dy = 0; dy < kBuildingFootprintTiles; ++dy) {
            for (int dx = 0; dx < kBuildingFootprintTiles; ++dx) {
                if (!free(site.x + dx, site.y + dy)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (ok(x, y)) {
        return Spawn{kind, side, x, y};
    }
    for (int r = 1; r < kMapTiles; ++r) {
        for (int ty = y - r; ty <= y + r; ++ty) {
            for (int tx = x - r; tx <= x + r; ++tx) {
                const bool on_ring = ty == y - r || ty == y + r || tx == x - r || tx == x + r;
                if (on_ring && ok(tx, ty)) {
                    return Spawn{kind, side, tx, ty};
                }
            }
        }
    }
    return Spawn{kind, side, x, y};
}

/// How many tiles on the straight line (x0,y0) -> (x1,y1) a tank could not enter.
int blocked_tiles(std::uint64_t seed, int x0, int y0, int x1, int y1) {
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    int blocked = 0;
    for (int i = 0; i <= steps; ++i) {
        const int x = x0 + dx * i / steps;
        const int y = y0 + dy * i / steps;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (!passable(seed, x + ox, y + oy)) {
                    ++blocked;
                }
            }
        }
    }
    return blocked;
}

} // namespace

std::vector<Objective> built_in_objectives() {
    // Loss first: a recycler pair that dies together reads as a loss.
    return {Objective{WarOutcome::PlayerLost, Side::Player, UnitKind::Recycler},
            Objective{WarOutcome::PlayerWon, Side::Enemy, UnitKind::Recycler}};
}

ScenarioData default_data() {
    // In spawn order (creation order is id order): tanks, the hover tank, the factory, the turrets,
    // the support weapons -- the mortar and the rocket -- then the recycler. The support weapons
    // come last so every unit before them keeps its id and tile; the newest kind is appended after
    // them for the same reason, and so that the recycler is the last id on each side. Offsets are
    // from the side's base centre and mirrored for the enemy so the two sides face each other:
    // the tanks lead, the turrets flank the factory, and the mortar and rocket stand BEHIND it, at
    // the rear of the cluster, where the tanks screen them. The recycler, the base on wheels,
    // stands at the rear beside the factory.
    ScenarioData data;
    data.groups = {
        {UnitKind::Tank, 1, 8, 2},       {UnitKind::Tank, 1, 8, 8},
        {UnitKind::Tank, 1, 2, 8},       {UnitKind::HoverTank, 1, 5, 5},
        {UnitKind::Factory, 1, 0, 0},    {UnitKind::Turret, 1, 5, -3},
        {UnitKind::Turret, 1, -3, 5},    {UnitKind::Mortar, 1, -4, -1},
        {UnitKind::Rocket, 1, -1, -4},   {UnitKind::Scavenger, 1, 1, -3},
        {UnitKind::Recycler, 1, -3, -3},
    };
    return data;
}

std::vector<UnitKind> side_roster(const ScenarioData& data, Side /*side*/) {
    std::vector<UnitKind> roster;
    for (const Group& group : data.groups) {
        roster.insert(roster.end(), static_cast<std::size_t>(group.count), group.kind);
    }
    return roster;
}

Scenario skirmish(std::uint64_t seed) { return skirmish(seed, default_data()); }

Scenario skirmish(std::uint64_t seed, const ScenarioData& data) {
    rng::SplitMix64 stream(rng::stream_seed(seed, kScenarioStream));
    const int jitter = static_cast<int>(kJitter);
    const int player_x = kEdge + static_cast<int>(stream.bounded(jitter));
    const int player_y = kEdge + static_cast<int>(stream.bounded(jitter));

    Scenario scenario{};
    scenario.seed = seed;
    scenario.objectives = data.objectives; // the mission's conditions travel with its composition
    for (const Group& group : data.groups) {
        for (int i = 0; i < group.count; ++i) {
            scenario.spawns.push_back(place(seed, scenario.spawns, group.kind, Side::Player,
                                            player_x + group.dx, player_y + group.dy));
        }
    }

    // The enemy base: the candidate (drawn from the same seeded stream) whose straight lines from
    // the player's tanks cross the fewest impassable tiles; the first perfectly clear one wins.
    int enemy_x = kEnemyEdge;
    int enemy_y = kEnemyEdge;
    int fewest = -1;
    for (int attempt = 0; attempt < kBaseAttempts && fewest != 0; ++attempt) {
        const int cx = kEnemyEdge + static_cast<int>(stream.bounded(jitter));
        const int cy = kEnemyEdge + static_cast<int>(stream.bounded(jitter));
        int blocked = 0;
        for (const Spawn& s : scenario.spawns) {
            if (is_tank(s.kind)) {
                blocked += blocked_tiles(seed, s.tile_x, s.tile_y, cx, cy);
            }
        }
        if (fewest < 0 || blocked < fewest) {
            fewest = blocked;
            enemy_x = cx;
            enemy_y = cy;
        }
    }
    for (const Group& group : data.groups) {
        for (int i = 0; i < group.count; ++i) {
            scenario.spawns.push_back(place(seed, scenario.spawns, group.kind, Side::Enemy,
                                            enemy_x - group.dx, enemy_y - group.dy));
        }
    }
    return scenario;
}

} // namespace protekt::sim
