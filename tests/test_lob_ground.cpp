#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/timings.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

/// M3: FIRE RESPECTS THE GROUND. High ground blocks weapons -- and the ARC still decides: a lob
/// that clears a ridge lands behind it (Harri: "I should still be able to lob a mortar over a
/// recycler to hit the target behind it, depending on the location").
///
/// The oracle for every case is the REAL flight (`launch` + `advance_projectiles`, no units to
/// stop it): where the shell actually comes down. `has_line` must agree with it, not with a
/// reasoning of its own.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;

[[nodiscard]] Unit unit_at(std::uint32_t id, UnitKind kind, Side side, int tx, int ty) {
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = Fixed::from_int(tx);
    unit.y = Fixed::from_int(ty);
    unit.hp = spec_of(kind).hp;
    unit.alive = true;
    return unit;
}

/// Where the shot from `gun` at `target` really comes down, flown with nothing in the way but the
/// ground: how far from the gun it lands against how far the target is (raw tiles). A shot that
/// never lands reports -1.
struct Touchdown {
    std::int64_t landed{-1};
    std::int64_t wanted{};
};

[[nodiscard]] Touchdown touchdown(const Unit& gun, const Unit& target) {
    std::vector<Unit> units{gun, target};
    units.at(1).alive = false; // a dead target stops nothing: only the ground does
    std::vector<Projectile> air{launch(gun, target, 10, kSeed)};
    std::vector<Impact> impacts;
    std::vector<Landing> landings;
    for (int tick = 0; tick < 400 && !air.empty(); ++tick) {
        advance_projectiles(air, units, impacts, landings, kSeed);
    }
    const auto run = [&](std::int64_t x, std::int64_t y) {
        const std::int64_t dx = x - gun.x.raw;
        const std::int64_t dy = y - gun.y.raw;
        return isqrt(dx * dx + dy * dy);
    };
    Touchdown out{};
    out.wanted = run(target.x.raw, target.y.raw);
    if (!landings.empty()) {
        out.landed = run(
            static_cast<std::int64_t>(landings.front().tile_x) * Fixed::kOne + Fixed::kOne / 2,
            static_cast<std::int64_t>(landings.front().tile_y) * Fixed::kOne + Fixed::kOne / 2);
    }
    return out;
}

[[nodiscard]] bool lands_on(const Unit& gun, const Unit& target) {
    const Touchdown t = touchdown(gun, target);
    return t.landed >= 0 && std::abs(t.landed - t.wanted) <= std::int64_t{2} * Fixed::kOne;
}
[[nodiscard]] bool lands_in_the_hill(const Unit& gun, const Unit& target) {
    const Touchdown t = touchdown(gun, target);
    return t.landed >= 0
           && t.landed < t.wanted - std::int64_t{3} * Fixed::kOne; // came down short of it
}

[[nodiscard]] int h(int x, int y) {
    return static_cast<int>(ground_height(kSeed, Fixed::from_int(x), Fixed::from_int(y)).raw
                            / Fixed::kOne);
}

struct Pair {
    int gx{}, gy{}, tx{}, ty{};
};

/// The first pair of passable tiles `lo`..`hi` tiles apart for which `want(gun, target)` holds.
template <typename Want>
[[nodiscard]] std::optional<Pair> find_pair(UnitKind gun_kind, int lo, int hi, Want want) {
    for (int gy = 20; gy < kMapTiles - 20; gy += 3) {
        for (int gx = 20; gx < kMapTiles - 20; gx += 3) {
            for (int dy = -hi; dy <= hi; dy += 1) {
                for (int dx = -hi; dx <= hi; dx += 1) {
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < lo * lo || d2 > hi * hi) {
                        continue;
                    }
                    const Unit gun = unit_at(1, gun_kind, Side::Player, gx, gy);
                    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, gx + dx, gy + dy);
                    if (want(gun, target)) {
                        return Pair{gx, gy, gx + dx, gy + dy};
                    }
                }
            }
        }
    }
    return std::nullopt;
}

/// A line of fire a flat gun could not have: the ground between is above a straight line.
[[nodiscard]] bool flat_line_blocked(const Unit& gun, const Unit& target) {
    return !can_see(kSeed, gun.x, gun.y, target.x, target.y, target.kind);
}

} // namespace

TEST(FireRespectsGround, AFlatShotAcrossARidgeIsRefusedAndWouldHaveLandedInTheHill) {
    const std::optional<Pair> found
        = find_pair(UnitKind::Tank, 6, 12, [](const Unit& gun, const Unit& target) {
              return flat_line_blocked(gun, target) && lands_in_the_hill(gun, target);
          });
    ASSERT_TRUE(found.has_value()) << "no ridge between two tiles on seed " << kSeed;
    const Pair p = found.value_or(Pair{});
    const Unit gun = unit_at(1, UnitKind::Tank, Side::Player, p.gx, p.gy);
    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, p.tx, p.ty);
    EXPECT_FALSE(has_line(gun, target, kSeed)) << "a tank fired through a hill";
}

TEST(FireRespectsGround, AShotOverClearGroundStillHasItsLine) {
    // The negative control: a rule that blocks everything is not a rule.
    const std::optional<Pair> found
        = find_pair(UnitKind::Tank, 6, 12, [](const Unit& gun, const Unit& target) {
              return !flat_line_blocked(gun, target) && lands_on(gun, target);
          });
    ASSERT_TRUE(found.has_value());
    const Pair p = found.value_or(Pair{});
    const Unit gun = unit_at(1, UnitKind::Tank, Side::Player, p.gx, p.gy);
    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, p.tx, p.ty);
    EXPECT_TRUE(has_line(gun, target, kSeed));
}

TEST(FireRespectsGround, ADownhillShotAtATargetBelowStillHasItsLine) {
    // A decline is not a block: the ground rises between, not away.
    const std::optional<Pair> found
        = find_pair(UnitKind::Tank, 6, 12, [](const Unit& gun, const Unit& target) {
              return h(static_cast<int>(gun.x.raw / Fixed::kOne),
                       static_cast<int>(gun.y.raw / Fixed::kOne))
                             - h(static_cast<int>(target.x.raw / Fixed::kOne),
                                 static_cast<int>(target.y.raw / Fixed::kOne))
                         >= 60
                     && !flat_line_blocked(gun, target) && lands_on(gun, target);
          });
    ASSERT_TRUE(found.has_value()) << "no downhill pair on seed " << kSeed;
    const Pair p = found.value_or(Pair{});
    const Unit gun = unit_at(1, UnitKind::Tank, Side::Player, p.gx, p.gy);
    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, p.tx, p.ty);
    EXPECT_TRUE(has_line(gun, target, kSeed));
}

TEST(FireRespectsGround, ALobThatClearsTheRidgeHasItsLineAndLandsBehindIt) {
    // HIS STANDING RULING, pinned: a flat gun has no line here (the ridge is in the way), and the
    // mortar's arc goes over it and comes down on the target.
    const std::optional<Pair> found
        = find_pair(UnitKind::MortarBike, 7, 14, [](const Unit& gun, const Unit& target) {
              return flat_line_blocked(gun, target) && lands_on(gun, target);
          });
    ASSERT_TRUE(found.has_value()) << "no lob-over-a-ridge pair on seed " << kSeed;
    const Pair p = found.value_or(Pair{});
    const Unit gun = unit_at(1, UnitKind::MortarBike, Side::Player, p.gx, p.gy);
    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, p.tx, p.ty);
    EXPECT_TRUE(has_line(gun, target, kSeed)) << "the arc clears the ridge, so the lob is allowed";
}

TEST(FireRespectsGround, ALobWhoseArcComesDownInTheHillHasNoLine) {
    // The arc is what is tested: the shell that would come down in the hill is declined, not
    // wasted.
    const std::optional<Pair> found
        = find_pair(UnitKind::MortarBike, 3, 14, [](const Unit& gun, const Unit& target) {
              return lands_in_the_hill(gun, target);
          });
    ASSERT_TRUE(found.has_value()) << "no lob lands in a hill on seed " << kSeed;
    const Pair p = found.value_or(Pair{});
    const Unit gun = unit_at(1, UnitKind::MortarBike, Side::Player, p.gx, p.gy);
    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, p.tx, p.ty);
    EXPECT_FALSE(has_line(gun, target, kSeed)) << "a mortar was cleared to lob into a hill";
}

TEST(FireRespectsGround, ALobOnClearGroundStillHasItsLine) {
    const std::optional<Pair> found
        = find_pair(UnitKind::MortarBike, 7, 14, [](const Unit& gun, const Unit& target) {
              return !flat_line_blocked(gun, target) && lands_on(gun, target);
          });
    ASSERT_TRUE(found.has_value());
    const Pair p = found.value_or(Pair{});
    const Unit gun = unit_at(1, UnitKind::MortarBike, Side::Player, p.gx, p.gy);
    const Unit target = unit_at(2, UnitKind::Tank, Side::Enemy, p.tx, p.ty);
    EXPECT_TRUE(has_line(gun, target, kSeed));
}

TEST(FireRespectsGround, ABlockedLobDoesNotMakeItsGunSearchEveryTick) {
    // THE BUDGET: the lane test was cut from 258 calls a tick to 44 (N9: a search every 15 ticks,
    // kept between beats). A lob refused for the ground must not undo that by being re-asked every
    // tick. Counted through the same `line_calls` the heavy-battle figure comes from.
    const std::optional<Pair> found
        = find_pair(UnitKind::MortarBike, 7, 14, [](const Unit& gun, const Unit& target) {
              return lands_in_the_hill(gun, target);
          });
    ASSERT_TRUE(found.has_value());
    const Pair p = found.value_or(Pair{});
    Scenario scenario{};
    scenario.seed = kSeed;
    Spawn gun{};
    gun.side = Side::Player;
    gun.kind = UnitKind::MortarBike;
    gun.tile_x = p.gx;
    gun.tile_y = p.gy;
    Spawn foe{};
    foe.side = Side::Enemy;
    foe.kind = UnitKind::Recycler;
    foe.tile_x = p.tx;
    foe.tile_y = p.ty;
    scenario.spawns = {gun, foe};
    Sim sim = Sim::create(scenario);
    std::size_t calls = 0;
    constexpr int kTicks = 90;
    for (int tick = 0; tick < kTicks; ++tick) {
        StepTimings timings{};
        sim.step(&timings);
        calls += timings.line_calls;
    }
    EXPECT_GT(calls, 0U) << "the gun never searched: the test counts nothing";
    EXPECT_LE(calls, static_cast<std::size_t>(kTicks / 15 + 1)) << "calls over " << kTicks;
}

} // namespace protekt::sim
