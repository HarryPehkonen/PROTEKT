#include "protekt/game/assist.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "protekt/game/attack_aim.hpp"
#include "protekt/sim/fixed.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

constexpr double kOne = static_cast<double>(sim::Fixed::kOne);
constexpr double kUnitsPerTile = static_cast<double>(sim::kHeightUnitsPerTile);
constexpr double kHalfStepDegrees = 0.25; ///< the mortar's elevation scan, before it is refined
constexpr int kRefinePasses = 40;         ///< bisection halvings of one scan step: ~1e-13 degrees
constexpr double kPi = 3.14159265358979323846;

/// A point in the sim's own units: the map plane in tiles and the height in TILES too (a rise of
/// one tile is 64 height units), so a direction in it needs no scaling.
struct Point {
    double x{};
    double y{};
    double z{};
};

[[nodiscard]] double tiles(sim::Fixed value) noexcept {
    return static_cast<double>(value.raw) / kOne;
}

[[nodiscard]] sim::Fixed fixed_of(double tiles_value) noexcept {
    const double limit = static_cast<double>(sim::kMapTiles - 1);
    return sim::Fixed::from_raw(
        static_cast<std::int32_t>(std::llround(std::clamp(tiles_value, 0.0, limit) * kOne)));
}

/// The sim's own ground under a point, in tiles of height.
[[nodiscard]] double ground_at(std::uint64_t seed, double x, double y) noexcept {
    return static_cast<double>(sim::ground_height(seed, fixed_of(x), fixed_of(y)).raw) / kOne
           / kUnitsPerTile;
}

/// Where a shot leaves the gun: the muzzle height over the ground under the shooter.
[[nodiscard]] Point muzzle_of(std::uint64_t seed, const sim::Unit& shooter) noexcept {
    const double x = tiles(shooter.x);
    const double y = tiles(shooter.y);
    return Point{x, y,
                 ground_at(seed, x, y)
                     + (static_cast<double>(sim::kMuzzleHeight.raw) / kOne / kUnitsPerTile)};
}

/// The target's middle `ticks` from now if it holds its velocity, standing on the ground there:
/// the point `launch` aims at, the middle of its body.
[[nodiscard]] Point middle_after(std::uint64_t seed, const sim::Unit& target,
                                 double ticks) noexcept {
    const double x = tiles(target.x) + (tiles(target.vx) * ticks);
    const double y = tiles(target.y) + (tiles(target.vy) * ticks);
    const double middle = (static_cast<double>(sim::body_bottom(target.kind).raw)
                           + static_cast<double>(sim::body_top(target.kind).raw))
                          * 0.5 / kOne / kUnitsPerTile;
    return Point{x, y, ground_at(seed, x, y) + middle};
}

/// One pass's answer: how long the shot flies to a point, and the unit direction it leaves along
/// (map x, map y, up).
struct Heading {
    double ticks{};
    std::array<double, 3> dir{};
};

/// The flat weapons. The bolt leaves at `speed` tiles a tick along the barrel, and the sim adds the
/// shooter's own map-plane velocity to it (`launch_towards`), so in the shooter's frame the target
/// has drifted by `-v_shooter * t` as well. Given the point and the time the pass assumed, the new
/// time is the distance over the speed.
[[nodiscard]] std::optional<Heading> flat_heading(const Point& from, const Point& at, double speed,
                                                  double travel, double drive_x, double drive_y,
                                                  double ticks) noexcept {
    const double dx = at.x - from.x - (drive_x * ticks);
    const double dy = at.y - from.y - (drive_y * ticks);
    const double dz = at.z - from.z;
    const double length = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    if (length < 1.0e-6 || speed <= 0.0 || length > travel) {
        return std::nullopt; // on top of the gun, or past where the bolt fizzles
    }
    return Heading{length / speed, {dx / length, dy / length, dz / length}};
}

/// The mortar. The shell leaves at ONE muzzle speed and the elevation divides it between reach and
/// climb (`mortar_climb`), then loses `gravity` after every step, so after n ticks it is `n *
/// across` along and `n * climb - gravity * n * (n - 1) / 2` up. For the horizontal run `run` and
/// the rise `rise` (tiles) the height at the target's range is a function of the elevation alone:
/// scanned from the steepest dive up, the FIRST place it climbs through the target's height is the
/// LOW arc, then bisected. No shooter velocity: `launch_lob` adds none.
[[nodiscard]] std::optional<Heading> lob_heading(const Point& from, const Point& at, double speed,
                                                 double gravity, double steepest,
                                                 double travel) noexcept {
    const double dx = at.x - from.x;
    const double dy = at.y - from.y;
    const double run = std::sqrt((dx * dx) + (dy * dy));
    const double rise = at.z - from.z;
    if (run < 1.0e-6 || speed <= 0.0) {
        return std::nullopt;
    }
    const auto miss_at = [&](double elevation) {
        const double ticks = run / (speed * std::cos(elevation));
        return (ticks * speed * std::sin(elevation)) - (0.5 * gravity * ticks * (ticks - 1.0))
               - rise;
    };
    const double step = kHalfStepDegrees * kPi / 180.0;
    for (double low = -steepest; low < steepest; low += step) {
        const double high = std::min(low + step, steepest);
        if (!(miss_at(low) < 0.0 && miss_at(high) >= 0.0)) {
            continue;
        }
        double under = low;
        double over = high;
        for (int pass = 0; pass < kRefinePasses; ++pass) {
            const double middle = (under + over) * 0.5;
            (miss_at(middle) < 0.0 ? under : over) = middle;
        }
        const double elevation = (under + over) * 0.5;
        const double across = speed * std::cos(elevation);
        const double ticks = run / across;
        if (ticks * across > travel) {
            return std::nullopt; // it fizzles in the air before it gets there
        }
        return Heading{
            ticks,
            {std::cos(elevation) * dx / run, std::cos(elevation) * dy / run, std::sin(elevation)}};
    }
    return std::nullopt; // out of reach at every elevation the barrel has
}

} // namespace

sim::UnitId assist_pick(const sim::Sim& sim, const SceneOptions& options) noexcept {
    return hostile_nearest_ray(sim, options, kAssistConeDegrees);
}

std::optional<AssistShot> assist_shot(const SceneOptions& options, Weapon weapon,
                                      const sim::Unit& shooter, const sim::Unit& target) noexcept {
    const std::uint64_t seed = options.seed;
    const Point from = muzzle_of(seed, shooter);
    const bool lob = weapon == Weapon::Mortar || weapon == Weapon::Bouncer;
    const sim::UnitSpec& mortar = sim::spec_of(sim::UnitKind::MortarBike);
    const sim::UnitSpec* flat = &sim::spec_of(shooter.kind);
    if (weapon == Weapon::Rocket) {
        flat = &sim::kRocketSpec;
    } else if (weapon == Weapon::Shotgun) {
        flat = &sim::kShotgunSpec;
    }
    if (!lob && flat->gravity.raw > 0) {
        return std::nullopt; // a flat weapon on a hull whose bolt falls: not a straight line to
                             // solve
    }
    const double drive_x = tiles(shooter.vx);
    const double drive_y = tiles(shooter.vy);

    // The iteration: aim at where the target will be after the flight time assumed so far, solve
    // the flight to that point for its real time, and go round again with that. It starts from the
    // target where it STANDS (no time), and stops when the time moves by less than
    // `kAssistSettleTicks`, or gives up after `kAssistMaxPasses`.
    double ticks = 0.0;
    for (int pass = 1; pass <= kAssistMaxPasses; ++pass) {
        const Point at = middle_after(seed, target, ticks);
        const std::optional<Heading> heading
            = lob ? lob_heading(
                        from, at, tiles(sim::Fixed::from_raw(mortar.bolt_speed.raw)),
                        static_cast<double>(mortar.gravity.raw) / kOne / kUnitsPerTile,
                        std::atan(tiles(mortar.arc)),
                        tiles(weapon == Weapon::Bouncer ? sim::kBouncerTravel : mortar.max_travel))
                  : flat_heading(from, at, tiles(flat->bolt_speed), tiles(flat->max_travel),
                                 drive_x, drive_y, ticks);
        if (!heading.has_value()) {
            return std::nullopt;
        }
        if (std::abs(heading->ticks - ticks) >= kAssistSettleTicks) {
            ticks = heading->ticks;
            continue;
        }
        // Settled. The sim's axes are (map x, map y, height); the camera's are (x, up, z) with the
        // map's y as z, and a tile is `kMetresPerTile` in all three.
        AssistShot shot{};
        shot.aim_metres
            = {static_cast<float>(at.x * kMetresPerTile), static_cast<float>(at.z * kMetresPerTile),
               static_cast<float>(at.y * kMetresPerTile)};
        shot.barrel
            = {static_cast<float>(heading->dir.at(0)), static_cast<float>(heading->dir.at(2)),
               static_cast<float>(heading->dir.at(1))};
        const std::array<float, 3> to{shot.aim_metres.at(0) - options.camera.x,
                                      shot.aim_metres.at(1) - options.camera.y,
                                      shot.aim_metres.at(2) - options.camera.z};
        const float reach
            = std::sqrt((to.at(0) * to.at(0)) + (to.at(1) * to.at(1)) + (to.at(2) * to.at(2)));
        shot.dot_metres = {options.camera.x + (shot.barrel.at(0) * reach),
                           options.camera.y + (shot.barrel.at(1) * reach),
                           options.camera.z + (shot.barrel.at(2) * reach)};
        shot.flight_ticks = static_cast<float>(heading->ticks);
        shot.passes = pass;
        return shot;
    }
    return std::nullopt; // it never settled: a target as fast as the shot has no lead
}

} // namespace protekt::game
