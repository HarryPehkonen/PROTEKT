#include "protekt/sim/projectile.hpp"

#include <algorithm>
#include <cstddef>

namespace protekt::sim {

namespace {

// The last representable position: just inside the map's far edge.
constexpr std::int64_t kMaxPosRaw = static_cast<std::int64_t>(kMapTiles) * Fixed::kOne - 1;

// Squared distance from the point (px, py) to the segment from (ax, ay) along (dx, dy). Positions
// are inside the map and a step is a fraction of a tile, so every product fits int64.
std::int64_t segment_dist2(std::int64_t ax, std::int64_t ay, std::int64_t dx, std::int64_t dy,
                           std::int64_t px, std::int64_t py) noexcept {
    const std::int64_t length2 = dx * dx + dy * dy;
    const std::int64_t along = (px - ax) * dx + (py - ay) * dy;
    std::int64_t cx = ax;
    std::int64_t cy = ay;
    if (length2 > 0 && along > 0) {
        if (along >= length2) {
            cx = ax + dx;
            cy = ay + dy;
        } else {
            cx = ax + dx * along / length2;
            cy = ay + dy * along / length2;
        }
    }
    return (px - cx) * (px - cx) + (py - cy) * (py - cy);
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

} // namespace

bool operator==(const Projectile& a, const Projectile& b) noexcept {
    return a.x == b.x && a.y == b.y && a.vx == b.vx && a.vy == b.vy && a.shooter == b.shooter
           && a.side == b.side && a.aimed_at == b.aimed_at && a.damage == b.damage
           && a.age == b.age;
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

Projectile launch(const Unit& shooter, const Unit& target, std::uint16_t damage) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(target.x.raw) - shooter.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(target.y.raw) - shooter.y.raw;
    const std::int64_t dist = isqrt(dx * dx + dy * dy);
    // A target exactly on the gun has no direction to aim in; due east is as good as any and keeps
    // the answer defined.
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
            continue;
        }

        const std::int64_t speed = isqrt(static_cast<std::int64_t>(bolt.vx.raw) * bolt.vx.raw
                                         + static_cast<std::int64_t>(bolt.vy.raw) * bolt.vy.raw);
        const bool out_of_range
            = static_cast<std::int64_t>(bolt.age) * speed >= kProjectileRange.raw;
        const bool off_map = to_x < 0 || to_y < 0 || to_x > kMaxPosRaw || to_y > kMaxPosRaw;
        if (speed == 0 || out_of_range || off_map) {
            continue; // fizzles: no damage, and no impact to report
        }
        bolt.x = Fixed::from_raw(static_cast<std::int32_t>(to_x));
        bolt.y = Fixed::from_raw(static_cast<std::int32_t>(to_y));
        projectiles[kept++] = bolt;
    }
    projectiles.resize(kept);
}

} // namespace protekt::sim
