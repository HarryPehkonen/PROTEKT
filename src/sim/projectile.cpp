#include "protekt/sim/projectile.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace protekt::sim {

namespace {

// The last representable position: just inside the map's far edge.
constexpr std::int64_t kMaxPosRaw = static_cast<std::int64_t>(kMapTiles) * Fixed::kOne - 1;

struct Point {
    std::int64_t x;
    std::int64_t y;
    std::int64_t along; ///< how far along the step it is, out of `length2`
    std::int64_t length2;
};

// The point on the segment from (ax, ay) along (dx, dy) nearest to (px, py), and how far along the
// segment it is (`along` out of `length2`, so a caller can find the bolt's height there). Positions
// are inside the map and a step is a fraction of a tile, so every product fits int64.
Point nearest_on_segment(std::int64_t ax, std::int64_t ay, std::int64_t dx, std::int64_t dy,
                         std::int64_t px, std::int64_t py) noexcept {
    const std::int64_t length2 = dx * dx + dy * dy;
    const std::int64_t along = (px - ax) * dx + (py - ay) * dy;
    Point c{ax, ay, 0, length2};
    if (length2 > 0 && along > 0) {
        if (along >= length2) {
            c = Point{ax + dx, ay + dy, length2, length2};
        } else {
            c = Point{ax + dx * along / length2, ay + dy * along / length2, along, length2};
        }
    }
    return c;
}

// Does the straight line from one point in the air to another stay clear of the ground, with a
// little to spare? Points are map positions (raw tiles) and heights (raw height units). The ground
// is looked at four times a tile, a good deal finer than a bolt's half-tile step.
bool line_clears_ground(std::uint64_t seed, std::int64_t ax, std::int64_t ay, std::int64_t az,
                        std::int64_t bx, std::int64_t by, std::int64_t bz) noexcept {
    constexpr std::int64_t kSamplesPerTile = 4;
    constexpr std::int64_t kClearance = std::int64_t{2} * Fixed::kOne; // height units
    const std::int64_t run = isqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    const std::int64_t samples = std::max<std::int64_t>(1, run * kSamplesPerTile / Fixed::kOne);
    for (std::int64_t i = 1; i < samples; ++i) {
        const Fixed x = Fixed::from_raw(static_cast<std::int32_t>(ax + (bx - ax) * i / samples));
        const Fixed y = Fixed::from_raw(static_cast<std::int32_t>(ay + (by - ay) * i / samples));
        if (ground_height(seed, x, y).raw + kClearance >= az + (bz - az) * i / samples) {
            return false;
        }
    }
    return true;
}

void take_damage(Unit& unit, std::uint16_t damage) noexcept {
    unit.hp = unit.hp > damage ? static_cast<std::uint16_t>(unit.hp - damage) : std::uint16_t{0};
    if (unit.hp == 0) {
        unit.alive = false;
        unit.stance = Stance::Dead;
        unit.vx = Fixed::zero();
        unit.vy = Fixed::zero();
    }
}

// Tracks the bolt's closest approach to the unit it was aimed at, over this tick's step, and says
// whether the bolt has now PASSED it: the nearest point of this step is not the step's end, so the
// distance has stopped falling and is rising again. A bolt that has passed has told its shooter
// everything it can. A target that is gone has nothing left to measure and nothing to pass.
bool track_approach(Projectile& bolt, const std::vector<Unit>& units, std::int64_t from_x,
                    std::int64_t from_y, std::int64_t step_x, std::int64_t step_y) noexcept {
    for (const Unit& unit : units) {
        if (unit.id != bolt.aimed_at) {
            continue;
        }
        if (!unit.alive) {
            return false; // the last approach stands
        }
        const Point c = nearest_on_segment(from_x, from_y, step_x, step_y, unit.x.raw, unit.y.raw);
        const std::int64_t mx = static_cast<std::int64_t>(unit.x.raw) - c.x;
        const std::int64_t my = static_cast<std::int64_t>(unit.y.raw) - c.y;
        const std::int64_t d2 = mx * mx + my * my;
        if (d2 < bolt.closest_d2) {
            bolt.closest_d2 = d2;
            bolt.miss_x = Fixed::from_raw(static_cast<std::int32_t>(mx));
            bolt.miss_y = Fixed::from_raw(static_cast<std::int32_t>(my));
        }
        return c.length2 > 0 && c.along < c.length2;
    }
    return false;
}

// The bolt has passed its target (or expired without having): the shooter adds the miss it saw to
// its aim estimate. The correction is the target's position when the bolt was nearest MINUS the
// bolt's, so it is measured from where the target WAS a flight-time ago: against a steady course it
// lands on the right lead, and against one that has changed it is wrong by exactly the change. That
// staleness is the evasion, not a flaw. It is about the map plane alone: a target's height does
// not evade, so the vertical aim has nothing to learn.
void learn_from(const Projectile& bolt, std::vector<Unit>& units) noexcept {
    if (!bolt.learns || bolt.closest_d2 == INT64_MAX) {
        return;
    }
    for (Unit& shooter : units) {
        if (shooter.id != bolt.shooter) {
            continue;
        }
        if (!shooter.alive || shooter.aim_target != bolt.aimed_at) {
            return; // dead, or it has moved on: one belief at a time, and this is not about it
        }
        std::int64_t x = static_cast<std::int64_t>(shooter.aim_x.raw) + bolt.miss_x.raw;
        std::int64_t y = static_cast<std::int64_t>(shooter.aim_y.raw) + bolt.miss_y.raw;
        const std::int64_t length = isqrt(x * x + y * y);
        if (length > kMaxAimLeadRaw) {
            x = x * kMaxAimLeadRaw / length;
            y = y * kMaxAimLeadRaw / length;
        }
        shooter.aim_x = Fixed::from_raw(static_cast<std::int32_t>(x));
        shooter.aim_y = Fixed::from_raw(static_cast<std::int32_t>(y));
        return;
    }
}

} // namespace

bool operator==(const Projectile& a, const Projectile& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.vx == b.vx && a.vy == b.vy && a.vz == b.vz
           && a.shooter == b.shooter && a.side == b.side && a.aimed_at == b.aimed_at
           && a.damage == b.damage && a.age == b.age && a.learns == b.learns
           && a.closest_d2 == b.closest_d2 && a.miss_x == b.miss_x && a.miss_y == b.miss_y;
}

std::int64_t isqrt(std::int64_t n) noexcept {
    if (n <= 0) {
        return 0;
    }
    auto rest = static_cast<std::uint64_t>(n);
    std::uint64_t root = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > rest) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (rest >= root + bit) {
            rest -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::int64_t>(root);
}

Fixed ground_height(std::uint64_t seed, Fixed x, Fixed y) noexcept {
    const std::int32_t mask = Fixed::kOne - 1;
    const int tx = x.floor_int();
    const int ty = y.floor_int();
    const std::int64_t fx = x.raw & mask;
    const std::int64_t fy = y.raw & mask;
    const std::int64_t h00 = world::sample(seed, tx, ty).height;
    const std::int64_t h10 = world::sample(seed, tx + 1, ty).height;
    const std::int64_t h01 = world::sample(seed, tx, ty + 1).height;
    const std::int64_t h11 = world::sample(seed, tx + 1, ty + 1).height;
    // Two planes per tile, split along the p00-p11 diagonal, exactly as the scene's mesh is.
    const std::int64_t raw = fx >= fy ? h00 * Fixed::kOne + (h10 - h00) * fx + (h11 - h10) * fy
                                      : h00 * Fixed::kOne + (h01 - h00) * fy + (h11 - h01) * fx;
    return Fixed::from_raw(static_cast<std::int32_t>(raw));
}

Fixed body_bottom(UnitKind /*kind*/) noexcept { return kRideHeight; }

Fixed body_top(UnitKind kind) noexcept {
    const Fixed body = kind == UnitKind::Factory ? kFactoryBodyHeight : kBodyHeight;
    return Fixed::from_raw(kRideHeight.raw + body.raw);
}

void retarget_aim(Unit& shooter, UnitId target) noexcept {
    if (shooter.aim_target != target) {
        shooter.aim_x = Fixed::zero();
        shooter.aim_y = Fixed::zero();
        shooter.aim_target = target;
    }
}

Projectile launch(const Unit& shooter, const Unit& target, std::uint16_t damage,
                  std::uint64_t seed) noexcept {
    // Aim at where the target IS, plus what this gun has learned about how far to lead it.
    const std::int64_t dx
        = static_cast<std::int64_t>(target.x.raw) - shooter.x.raw + shooter.aim_x.raw;
    const std::int64_t dy
        = static_cast<std::int64_t>(target.y.raw) - shooter.y.raw + shooter.aim_y.raw;
    const std::int64_t dist = isqrt(dx * dx + dy * dy);
    // An aim point exactly on the gun has no direction to aim in; due east is as good as any and
    // keeps the answer defined.
    std::int64_t aim_x = kProjectileSpeedRaw;
    std::int64_t aim_y = 0;
    if (dist > 0) {
        aim_x = dx * kProjectileSpeedRaw / dist;
        aim_y = dy * kProjectileSpeedRaw / dist;
    }
    Projectile bolt{};
    bolt.x = shooter.x;
    bolt.y = shooter.y;
    bolt.z = Fixed::from_raw(ground_height(seed, shooter.x, shooter.y).raw + kMuzzleHeight.raw);
    bolt.vx = Fixed::from_raw(static_cast<std::int32_t>(aim_x + shooter.vx.raw));
    bolt.vy = Fixed::from_raw(static_cast<std::int32_t>(aim_y + shooter.vy.raw));

    // The vertical aim is exact: the aim POINT is on the ground it is over, and a body stands on
    // it. With no estimate that is the target where it really stands; with one, it is where the
    // target is expected to be, at the height the ground has THERE, so a target walking up a slope
    // is aimed at on the slope. There is no vertical estimate to learn or to be wrong: the height
    // follows from the horizontal aim. Within the body the gun takes the lowest point it can see --
    // the middle, or higher if a rise in the ground would stop a bolt on its way to the middle --
    // since a shot that stops short of a target on a hillside teaches nothing and repeats forever.
    // The rise is spread over the run to the aim point.
    if (dist > 0) {
        const std::int64_t point_x = static_cast<std::int64_t>(shooter.x.raw) + dx;
        const std::int64_t point_y = static_cast<std::int64_t>(shooter.y.raw) + dy;
        const std::int64_t stands
            = ground_height(seed, Fixed::from_raw(static_cast<std::int32_t>(point_x)),
                            Fixed::from_raw(static_cast<std::int32_t>(point_y)))
                  .raw;
        const std::int64_t low = body_bottom(target.kind).raw;
        const std::int64_t height = static_cast<std::int64_t>(body_top(target.kind).raw) - low;
        std::int64_t aim_z = 0;
        for (const std::int64_t sixteenths : {8, 12, 15}) {
            aim_z = stands + low + height * sixteenths / 16;
            if (line_clears_ground(seed, shooter.x.raw, shooter.y.raw, bolt.z.raw, point_x, point_y,
                                   aim_z)) {
                break; // otherwise the last, highest one stands
            }
        }
        // The rise is spread over the bolt's real closing speed on the aim point -- the gun's own
        // drive adds to it or takes from it -- not over the speed it was aimed at, or a moving
        // gun's bolt would arrive early and low (or late and high).
        const std::int64_t closing = (static_cast<std::int64_t>(bolt.vx.raw) * dx
                                      + static_cast<std::int64_t>(bolt.vy.raw) * dy)
                                     / dist;
        const std::int64_t speed = closing > 0 ? closing : kProjectileSpeedRaw;
        const std::int64_t steepest = std::int64_t{2} * kProjectileSpeedRaw * kHeightUnitsPerTile;
        const std::int64_t vz
            = std::clamp((aim_z - bolt.z.raw) * speed / dist, -steepest, steepest);
        bolt.vz = Fixed::from_raw(static_cast<std::int32_t>(vz));
    }
    bolt.shooter = shooter.id;
    bolt.side = shooter.side;
    bolt.aimed_at = target.id;
    bolt.damage = damage;
    bolt.learns = true;
    return bolt;
}

Projectile launch_towards(const Unit& shooter, Fixed dir_x, Fixed dir_y, Fixed dir_z,
                          UnitId aimed_at, std::uint16_t damage, std::uint64_t seed) noexcept {
    std::int64_t dx = dir_x.raw;
    std::int64_t dy = dir_y.raw;
    std::int64_t dz = dir_z.raw;
    // Three squares of a 32-bit value can overflow 64 bits, and a replay may carry any value: only
    // the direction matters, so a huge one is scaled down first.
    const std::int64_t largest = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
    if (largest > (std::int64_t{1} << 28)) {
        dx >>= 8;
        dy >>= 8;
        dz >>= 8;
    }
    const std::int64_t length = isqrt(dx * dx + dy * dy + dz * dz);
    std::int64_t aim_x = kProjectileSpeedRaw;
    std::int64_t aim_y = 0;
    std::int64_t aim_z = 0;
    if (length > 0) {
        aim_x = dx * kProjectileSpeedRaw / length;
        aim_y = dy * kProjectileSpeedRaw / length;
        // The same direction, the same speed: a rise of one tile is 64 height units.
        aim_z = dz * kProjectileSpeedRaw * kHeightUnitsPerTile / length;
    }
    Projectile bolt{};
    bolt.x = shooter.x;
    bolt.y = shooter.y;
    bolt.z = Fixed::from_raw(ground_height(seed, shooter.x, shooter.y).raw + kMuzzleHeight.raw);
    bolt.vx = Fixed::from_raw(static_cast<std::int32_t>(aim_x + shooter.vx.raw));
    bolt.vy = Fixed::from_raw(static_cast<std::int32_t>(aim_y + shooter.vy.raw));
    bolt.vz = Fixed::from_raw(static_cast<std::int32_t>(aim_z));
    bolt.shooter = shooter.id;
    bolt.side = shooter.side;
    bolt.aimed_at = aimed_at; // intent only: the velocity above never looked at it
    bolt.damage = damage;
    return bolt;
}

void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts, std::uint64_t seed) noexcept {
    const std::int64_t radius2
        = static_cast<std::int64_t>(kProjectileHitRadius.raw) * kProjectileHitRadius.raw;
    std::size_t kept = 0;
    // Index order is spawn order. A unit killed by an earlier bolt in this loop is dead for the
    // later ones, so the order bolts were fired in decides who lands the killing blow.
    for (std::size_t i = 0; i < projectiles.size(); ++i) {
        Projectile bolt = projectiles[i];
        const std::int64_t from_x = bolt.x.raw;
        const std::int64_t from_y = bolt.y.raw;
        const std::int64_t from_z = bolt.z.raw;
        std::int64_t step_x = bolt.vx.raw;
        std::int64_t step_y = bolt.vy.raw;
        std::int64_t step_z = bolt.vz.raw;
        ++bolt.age;

        // Where the step ends. If that is in the ground, the bolt flies only as far as the ground:
        // the fraction of the step before the two heights cross, found along the straight line
        // between them.
        const std::int64_t to_x = from_x + step_x;
        const std::int64_t to_y = from_y + step_y;
        const std::int64_t end_clearance
            = from_z + step_z
              - ground_height(seed, Fixed::from_raw(static_cast<std::int32_t>(to_x)),
                              Fixed::from_raw(static_cast<std::int32_t>(to_y)))
                    .raw;
        const bool grounded = end_clearance <= 0;
        if (grounded) {
            const std::int64_t start_clearance = from_z - ground_height(seed, bolt.x, bolt.y).raw;
            const std::int64_t until = start_clearance > 0 ? start_clearance : 0;
            const std::int64_t whole = until - end_clearance; // end_clearance <= 0 <= until
            if (whole > 0) {
                step_x = step_x * until / whole;
                step_y = step_y * until / whole;
                step_z = step_z * until / whole;
            } else {
                step_x = 0; // already on the ground, and going nowhere
                step_y = 0;
                step_z = 0;
            }
        }

        const bool passed
            = bolt.learns && track_approach(bolt, units, from_x, from_y, step_x, step_y);

        Unit* hit = nullptr;
        std::int64_t hit_d2 = radius2 + 1;
        for (Unit& unit : units) { // ascending id, so a tie keeps the lowest
            if (!unit.alive || (unit.side == bolt.side && unit.id != bolt.aimed_at)) {
                continue;
            }
            const Point c
                = nearest_on_segment(from_x, from_y, step_x, step_y, unit.x.raw, unit.y.raw);
            const std::int64_t d2
                = (unit.x.raw - c.x) * (unit.x.raw - c.x) + (unit.y.raw - c.y) * (unit.y.raw - c.y);
            if (d2 > radius2 || d2 >= hit_d2) {
                continue;
            }
            // Close enough in the map plane; now is the bolt level with the body, at that moment?
            const std::int64_t z = c.length2 > 0 ? from_z + step_z * c.along / c.length2 : from_z;
            const std::int64_t stands = ground_height(seed, unit.x, unit.y).raw;
            if (z >= stands + body_bottom(unit.kind).raw && z <= stands + body_top(unit.kind).raw) {
                hit = &unit;
                hit_d2 = d2;
            }
        }
        if (hit != nullptr) {
            take_damage(*hit, bolt.damage);
            impacts.push_back(Impact{bolt.shooter, hit->id, hit->hp});
            continue; // a hit teaches nothing: there was nothing to correct
        }
        if (passed) {
            learn_from(bolt, units); // it has told the shooter all it can; not again at the end
            bolt.learns = false;
        }
        if (grounded) {
            continue; // gone where it met the ground: no damage, no mark, no lesson
        }

        const std::int64_t vz_tiles = static_cast<std::int64_t>(bolt.vz.raw) / kHeightUnitsPerTile;
        const std::int64_t speed
            = isqrt(static_cast<std::int64_t>(bolt.vx.raw) * bolt.vx.raw
                    + static_cast<std::int64_t>(bolt.vy.raw) * bolt.vy.raw + vz_tiles * vz_tiles);
        const bool out_of_range
            = static_cast<std::int64_t>(bolt.age) * speed >= kProjectileRange.raw;
        const bool off_map = to_x < 0 || to_y < 0 || to_x > kMaxPosRaw || to_y > kMaxPosRaw;
        if (speed == 0 || out_of_range || off_map) {
            learn_from(bolt, units);
            continue; // fizzles: no damage, and no impact to report
        }
        bolt.x = Fixed::from_raw(static_cast<std::int32_t>(to_x));
        bolt.y = Fixed::from_raw(static_cast<std::int32_t>(to_y));
        bolt.z = Fixed::from_raw(static_cast<std::int32_t>(from_z + step_z));
        projectiles[kept++] = bolt;
    }
    projectiles.resize(kept);
}

} // namespace protekt::sim
