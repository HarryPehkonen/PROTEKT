#include "protekt/sim/projectile.hpp"

#include <algorithm>
#include <cstddef>

namespace protekt::sim {

namespace {

// The last representable position: just inside the map's far edge.
constexpr std::int64_t kMaxPosRaw = static_cast<std::int64_t>(kMapTiles) * Fixed::kOne - 1;

struct Point {
    std::int64_t x;
    std::int64_t y;
};

// The point on the segment from (ax, ay) along (dx, dy) nearest to (px, py). Positions are inside
// the map and a step is a fraction of a tile, so every product fits int64.
Point nearest_on_segment(std::int64_t ax, std::int64_t ay, std::int64_t dx, std::int64_t dy,
                         std::int64_t px, std::int64_t py) noexcept {
    const std::int64_t length2 = dx * dx + dy * dy;
    const std::int64_t along = (px - ax) * dx + (py - ay) * dy;
    Point c{ax, ay};
    if (length2 > 0 && along > 0) {
        if (along >= length2) {
            c = Point{ax + dx, ay + dy};
        } else {
            c = Point{ax + dx * along / length2, ay + dy * along / length2};
        }
    }
    return c;
}

// Squared distance from the point (px, py) to the segment from (ax, ay) along (dx, dy).
std::int64_t segment_dist2(std::int64_t ax, std::int64_t ay, std::int64_t dx, std::int64_t dy,
                           std::int64_t px, std::int64_t py) noexcept {
    const Point c = nearest_on_segment(ax, ay, dx, dy, px, py);
    return (px - c.x) * (px - c.x) + (py - c.y) * (py - c.y);
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

// Tracks the bolt's closest approach to the unit it was aimed at, over this tick's step.
void track_approach(Projectile& bolt, const std::vector<Unit>& units, std::int64_t from_x,
                    std::int64_t from_y) noexcept {
    for (const Unit& unit : units) {
        if (unit.id != bolt.aimed_at) {
            continue;
        }
        if (!unit.alive) {
            return; // nothing left to measure against; the last approach stands
        }
        const Point c
            = nearest_on_segment(from_x, from_y, bolt.vx.raw, bolt.vy.raw, unit.x.raw, unit.y.raw);
        const std::int64_t mx = static_cast<std::int64_t>(unit.x.raw) - c.x;
        const std::int64_t my = static_cast<std::int64_t>(unit.y.raw) - c.y;
        const std::int64_t d2 = mx * mx + my * my;
        if (d2 < bolt.closest_d2) {
            bolt.closest_d2 = d2;
            bolt.miss_x = Fixed::from_raw(static_cast<std::int32_t>(mx));
            bolt.miss_y = Fixed::from_raw(static_cast<std::int32_t>(my));
        }
        return;
    }
}

// The shot fizzled: the shooter adds the miss it saw to its aim estimate. The correction is the
// target's position when the bolt was nearest MINUS the bolt's, so it is measured from where the
// target WAS a flight-time ago: against a steady course it lands on the right lead, and against one
// that has changed it is wrong by exactly the change. That staleness is the evasion, not a flaw.
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
    return a.x == b.x && a.y == b.y && a.vx == b.vx && a.vy == b.vy && a.shooter == b.shooter
           && a.side == b.side && a.aimed_at == b.aimed_at && a.damage == b.damage && a.age == b.age
           && a.learns == b.learns && a.closest_d2 == b.closest_d2 && a.miss_x == b.miss_x
           && a.miss_y == b.miss_y;
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

void retarget_aim(Unit& shooter, UnitId target) noexcept {
    if (shooter.aim_target != target) {
        shooter.aim_x = Fixed::zero();
        shooter.aim_y = Fixed::zero();
        shooter.aim_target = target;
    }
}

Projectile launch(const Unit& shooter, const Unit& target, std::uint16_t damage) noexcept {
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
    bolt.vx = Fixed::from_raw(static_cast<std::int32_t>(aim_x + shooter.vx.raw));
    bolt.vy = Fixed::from_raw(static_cast<std::int32_t>(aim_y + shooter.vy.raw));
    bolt.shooter = shooter.id;
    bolt.side = shooter.side;
    bolt.aimed_at = target.id;
    bolt.damage = damage;
    bolt.learns = true;
    return bolt;
}

Projectile launch_towards(const Unit& shooter, Fixed dir_x, Fixed dir_y, UnitId aimed_at,
                          std::uint16_t damage) noexcept {
    const std::int64_t dx = dir_x.raw;
    const std::int64_t dy = dir_y.raw;
    const std::int64_t length = isqrt(dx * dx + dy * dy);
    std::int64_t aim_x = kProjectileSpeedRaw;
    std::int64_t aim_y = 0;
    if (length > 0) {
        aim_x = dx * kProjectileSpeedRaw / length;
        aim_y = dy * kProjectileSpeedRaw / length;
    }
    Projectile bolt{};
    bolt.x = shooter.x;
    bolt.y = shooter.y;
    bolt.vx = Fixed::from_raw(static_cast<std::int32_t>(aim_x + shooter.vx.raw));
    bolt.vy = Fixed::from_raw(static_cast<std::int32_t>(aim_y + shooter.vy.raw));
    bolt.shooter = shooter.id;
    bolt.side = shooter.side;
    bolt.aimed_at = aimed_at; // intent only: the velocity above never looked at it
    bolt.damage = damage;
    return bolt;
}

void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts) noexcept {
    const std::int64_t radius2
        = static_cast<std::int64_t>(kProjectileHitRadius.raw) * kProjectileHitRadius.raw;
    std::size_t kept = 0;
    // Index order is spawn order. A unit killed by an earlier bolt in this loop is dead for the
    // later ones, so the order bolts were fired in decides who lands the killing blow.
    for (std::size_t i = 0; i < projectiles.size(); ++i) {
        Projectile bolt = projectiles[i];
        const std::int64_t from_x = bolt.x.raw;
        const std::int64_t from_y = bolt.y.raw;
        const std::int64_t to_x = from_x + bolt.vx.raw;
        const std::int64_t to_y = from_y + bolt.vy.raw;
        ++bolt.age;
        if (bolt.learns) {
            track_approach(bolt, units, from_x, from_y);
        }

        Unit* hit = nullptr;
        std::int64_t hit_d2 = radius2 + 1;
        for (Unit& unit : units) { // ascending id, so a tie keeps the lowest
            if (!unit.alive || (unit.side == bolt.side && unit.id != bolt.aimed_at)) {
                continue;
            }
            const std::int64_t d2
                = segment_dist2(from_x, from_y, bolt.vx.raw, bolt.vy.raw, unit.x.raw, unit.y.raw);
            if (d2 <= radius2 && d2 < hit_d2) {
                hit = &unit;
                hit_d2 = d2;
            }
        }
        if (hit != nullptr) {
            take_damage(*hit, bolt.damage);
            impacts.push_back(Impact{bolt.shooter, hit->id, hit->hp});
            continue; // a hit teaches nothing: there was nothing to correct
        }

        const std::int64_t speed = isqrt(static_cast<std::int64_t>(bolt.vx.raw) * bolt.vx.raw
                                         + static_cast<std::int64_t>(bolt.vy.raw) * bolt.vy.raw);
        const bool out_of_range
            = static_cast<std::int64_t>(bolt.age) * speed >= kProjectileRange.raw;
        const bool off_map = to_x < 0 || to_y < 0 || to_x > kMaxPosRaw || to_y > kMaxPosRaw;
        if (speed == 0 || out_of_range || off_map) {
            learn_from(bolt, units);
            continue; // fizzles: no damage, and no impact to report
        }
        bolt.x = Fixed::from_raw(static_cast<std::int32_t>(to_x));
        bolt.y = Fixed::from_raw(static_cast<std::int32_t>(to_y));
        projectiles[kept++] = bolt;
    }
    projectiles.resize(kept);
}

} // namespace protekt::sim
