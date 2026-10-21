#include "protekt/sim/scenario.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <span>

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

struct Offset {
    int dx;
    int dy;
};

/// The roster's kinds, in spawn order (creation order is id order): tanks, the hover tank, the
/// factory, then the turrets. Both sides spawn the same list.
constexpr std::array<UnitKind, 7> kRoster{{
    UnitKind::Tank,
    UnitKind::Tank,
    UnitKind::Tank,
    UnitKind::HoverTank,
    UnitKind::Factory,
    UnitKind::Turret,
    UnitKind::Turret,
}};

/// Offsets from the side's base centre, parallel to kRoster; mirrored for the enemy so the two
/// sides face each other.
constexpr std::array<Offset, 7> kOffsets{{
    {8, 2},
    {8, 8},
    {2, 8},
    {5, 5},
    {0, 0},
    {5, -3},
    {-3, 5},
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

std::span<const UnitKind> side_roster(Side /*side*/) noexcept { return kRoster; }

Scenario skirmish(std::uint64_t seed) {
    rng::SplitMix64 stream(rng::stream_seed(seed, kScenarioStream));
    const int jitter = static_cast<int>(kJitter);
    const int player_x = kEdge + static_cast<int>(stream.bounded(jitter));
    const int player_y = kEdge + static_cast<int>(stream.bounded(jitter));

    Scenario scenario{};
    scenario.seed = seed;
    scenario.spawns.reserve(2 * kRoster.size());
    const std::span<const UnitKind> player_roster = side_roster(Side::Player);
    for (std::size_t i = 0; i < player_roster.size(); ++i) {
        scenario.spawns.push_back(place(seed, scenario.spawns, player_roster[i], Side::Player,
                                        player_x + kOffsets.at(i).dx,
                                        player_y + kOffsets.at(i).dy));
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
    const std::span<const UnitKind> enemy_roster = side_roster(Side::Enemy);
    for (std::size_t i = 0; i < enemy_roster.size(); ++i) {
        scenario.spawns.push_back(place(seed, scenario.spawns, enemy_roster[i], Side::Enemy,
                                        enemy_x - kOffsets.at(i).dx, enemy_y - kOffsets.at(i).dy));
    }
    return scenario;
}

} // namespace protekt::sim
