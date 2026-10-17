#include "protekt/sim/scenario.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

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

struct Slot {
    UnitKind kind;
    int dx;
    int dy;
};

/// Creation order is id order: tanks, then the factory, then the turrets. Offsets are mirrored for
/// the enemy so the two sides face each other.
constexpr std::array<Slot, 6> kComposition{{
    {UnitKind::Tank, 8, 2},
    {UnitKind::Tank, 8, 8},
    {UnitKind::Tank, 2, 8},
    {UnitKind::Factory, 0, 0},
    {UnitKind::Turret, 5, -3},
    {UnitKind::Turret, -3, 5},
}};

bool passable(std::uint64_t seed, int x, int y) {
    return x >= 0 && y >= 0 && x < kMapTiles && y < kMapTiles
           && terrain_speed_factor(world::sample(seed, x, y).terrain).raw != 0;
}

bool taken(const std::vector<Spawn>& spawns, int x, int y) {
    return std::any_of(spawns.begin(), spawns.end(),
                       [x, y](const Spawn& s) { return s.tile_x == x && s.tile_y == y; });
}

/// The wanted tile if free and passable, otherwise the first one found on growing square rings in
/// row-major order. Falls back to the clamped wish if the whole map is somehow blocked.
Spawn place(std::uint64_t seed, const std::vector<Spawn>& spawns, UnitKind kind, Side side, int x,
            int y) {
    x = std::clamp(x, 0, kMapTiles - 1);
    y = std::clamp(y, 0, kMapTiles - 1);
    const auto ok
        = [&](int tx, int ty) { return passable(seed, tx, ty) && !taken(spawns, tx, ty); };
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

Scenario skirmish(std::uint64_t seed) {
    rng::SplitMix64 stream(rng::stream_seed(seed, kScenarioStream));
    const int jitter = static_cast<int>(kJitter);
    const int player_x = kEdge + static_cast<int>(stream.bounded(jitter));
    const int player_y = kEdge + static_cast<int>(stream.bounded(jitter));

    Scenario scenario{};
    scenario.seed = seed;
    scenario.spawns.reserve(2 * kComposition.size());
    for (const Slot& slot : kComposition) {
        scenario.spawns.push_back(place(seed, scenario.spawns, slot.kind, Side::Player,
                                        player_x + slot.dx, player_y + slot.dy));
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
            if (s.kind == UnitKind::Tank) {
                blocked += blocked_tiles(seed, s.tile_x, s.tile_y, cx, cy);
            }
        }
        if (fewest < 0 || blocked < fewest) {
            fewest = blocked;
            enemy_x = cx;
            enemy_y = cy;
        }
    }
    for (const Slot& slot : kComposition) {
        scenario.spawns.push_back(place(seed, scenario.spawns, slot.kind, Side::Enemy,
                                        enemy_x - slot.dx, enemy_y - slot.dy));
    }
    return scenario;
}

} // namespace protekt::sim
