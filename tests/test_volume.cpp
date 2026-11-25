#include "protekt/sim/orders.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <vector>

/// SLICE M5: THE VOLUMES, AND THE JUMP. The owner, asked whether the E boost should become a jump
/// and what it takes: "Yes -- and SPEED is what it takes: below a threshold you just bump the
/// obstacle; above it, you clear it." And his map spec, which the volumes are FOR: "valleys that
/// limit movement, ramps that enable movement to higher parts of the map, high parts that block
/// weapons ... places where you can easily fall downhill but impossible to go back unless there
/// is a ramp."
///
/// A solid thing is three numbers -- an extent, a base, a top -- and a unit at height h passes
/// where no volume blocks at h. A jumped unit has a height, so the wall no longer stops it.
///
/// The rig: a WALL one tile deep, 27 tiles wide (the whole flat strip of seed 42, x 56 .. 82),
/// on row kWallY, set down as terrain volumes (`Scenario::volumes`); a tank 1.5 tiles south of it
/// on column 69, ordered to a goal on the far side. Heights are in height units
/// (`kHeightUnitsPerTile` to a tile): a LOW wall's top is 40, a TALL one's 120; a tank's ride
/// height is 8 and its body 57.6.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = Fixed::kOne;
constexpr int kWallY = 29;
constexpr int kWallLeft = 56;
constexpr int kWallWidth = 27;
constexpr int kStartX = 69;
constexpr int kStartY = 27;
constexpr int kGoalY = 34;
constexpr int kRunTicks = 45;

[[nodiscard]] bool open_ground() {
    for (int y = 22; y <= 35; ++y) {
        for (int x = 56; x <= 82; ++x) {
            if (world::sample(kSeed, x, y).terrain != world::Terrain::Flat) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Fixed units_of(int height) { return Fixed::from_int(height); }

/// A wall of the given top, one tile deep along row kWallY, `kWallWidth` wide: eight-wide pieces
/// (a mask holds at most 64 tiles) set side by side from kWallLeft.
[[nodiscard]] std::vector<PlacedVolume> wall(int base, int top) {
    std::vector<PlacedVolume> out;
    for (int x = kWallLeft; x < kWallLeft + kWallWidth; x += 8) {
        const int width = std::min(8, kWallLeft + kWallWidth - x);
        const SolidMask row{width, 1, (std::uint64_t{1} << static_cast<unsigned>(width)) - 1};
        out.push_back(PlacedVolume{x, kWallY, false, Volume{row, units_of(base), units_of(top)}});
    }
    return out;
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, int y) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    return s;
}

[[nodiscard]] Sim rig(UnitKind kind, std::vector<PlacedVolume> volumes) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = {spawn(kind, Side::Player, kStartX, kStartY),
                       spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    scenario.volumes = std::move(volumes);
    return Sim::create(scenario);
}

/// One tick's record of the unit under test: where it is and how high.
struct Frame {
    int tick;
    double x;
    double y;
    double lift;
};

struct Flight {
    std::vector<Frame> frames;
    bool ever_airborne{};
    double max_y{};
    double final_lift{};
};

/// Drives the unit at the goal across the wall. `jump_after` is the tick after which the jump is
/// asked for (a run-up of that many ticks first); negative asks for none. Prints nothing.
[[nodiscard]] Flight drive(Sim& sim, int jump_after) {
    const UnitId id = sim.units().at(0).id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(kStartX), Fixed::from_int(kGoalY)}});
    Flight flight{};
    for (int t = 0; t < kRunTicks; ++t) {
        if (t == jump_after) {
            sim.enqueue(Order{id, orders::Jump{}});
        }
        sim.step();
        const Unit& u = *sim.find(id);
        const double x = static_cast<double>(u.x.raw) / kTile;
        const double y = static_cast<double>(u.y.raw) / kTile;
        const double lift = static_cast<double>(u.lift.raw) / kTile;
        flight.frames.push_back(Frame{static_cast<int>(sim.tick()), x, y, lift});
        flight.ever_airborne = flight.ever_airborne || airborne(u);
        flight.max_y = std::max(flight.max_y, y);
        flight.final_lift = lift;
    }
    return flight;
}

void print(const char* label, const Flight& flight) {
    std::cout << std::format("[ M5 ] {}\n[ M5 ]   tick   x(tiles)   y(tiles)   lift(units)\n",
                             label);
    for (const Frame& f : flight.frames) {
        if (f.lift != 0.0 || f.tick <= 3 || f.tick % 5 == 0) {
            std::cout << std::format("[ M5 ]   {:4}   {:8.3f}   {:8.3f}   {:8.3f}\n", f.tick, f.x,
                                     f.y, f.lift);
        }
    }
}

[[nodiscard]] Unit placed(UnitKind kind, int x_raw, int y_raw) {
    Unit unit{};
    unit.id = UnitId{1};
    unit.kind = kind;
    unit.side = Side::Player;
    unit.x = Fixed::from_raw(x_raw);
    unit.y = Fixed::from_raw(y_raw);
    unit.hp = spec_of(kind).hp;
    return unit;
}

// --- THE RULE: a unit at height h passes a LOW volume and not a TALL one -------------------------

TEST(Volume, AUnitAtHeightPassesOverALowVolumeAndNotOverATallOne) {
    const Volume low{SolidMask{1, 1, 1}, units_of(0), units_of(40)};
    const Volume tall{SolidMask{1, 1, 1}, units_of(0), units_of(120)};
    // The body's feet are at ride height (8) plus the lift: the wall stops a unit whose feet are
    // below its top. A lip of 40 is cleared from a lift of 32; the jump's whole peak is 55.
    EXPECT_TRUE(volume_stops(low, units_of(0))) << "on the ground, a wall is a wall";
    EXPECT_TRUE(volume_stops(low, units_of(31)));
    EXPECT_FALSE(volume_stops(low, units_of(32))) << "feet exactly on the top: over";
    EXPECT_FALSE(volume_stops(low, units_of(55)));
    EXPECT_TRUE(volume_stops(tall, units_of(55))) << "the jump's own peak does not clear 120";
    EXPECT_TRUE(volume_stops(tall, units_of(111)));
    EXPECT_FALSE(volume_stops(tall, units_of(112)));
}

TEST(Volume, ABridgeLetsAUnderPassAndAUnitOnTopDriveOver) {
    // A deck from 70 to 78 above the ground: a tank's head is at 8 + 57.6 = 65.6, so it drives
    // UNDER; lifted to the deck it drives ON TOP; in between it is inside it.
    const Volume deck{SolidMask{1, 1, 1}, units_of(70), units_of(78)};
    EXPECT_FALSE(volume_stops(deck, units_of(0)));
    EXPECT_FALSE(volume_stops(deck, units_of(4)));
    EXPECT_TRUE(volume_stops(deck, units_of(5)));
    EXPECT_TRUE(volume_stops(deck, units_of(69)));
    EXPECT_FALSE(volume_stops(deck, units_of(70)));
}

TEST(Volume, ABuildingIsAVolumeAndItsRoofIsFarAboveAJump) {
    const Volume spire = volume_of(UnitKind::GunSpire);
    EXPECT_EQ(spire.base.raw, 0);
    EXPECT_EQ(spire.top.raw, kRideHeight.raw + kBodyHeight.raw * kBuildingFootprintScale);
    EXPECT_GT(spire.top.raw, units_of(55).raw + kRideHeight.raw) << "no jump clears a building";
    const Unit one = placed(UnitKind::GunSpire, 62 * kTile, 28 * kTile);
    const std::array<Unit, 1> units{one};
    EXPECT_TRUE(building_stops_units(units, 60, 26, units_of(55))) << "a prong, at the jump's peak";
    EXPECT_TRUE(building_stops_units(units, 60, 26)) << "and on the ground, as ever";
}

// --- THE MASK BECAME THE EXTENT: the prongs still drive between
// -----------------------------------

TEST(Volume, TheMaskIsTheVolumesExtentNotASecondIdea) {
    EXPECT_EQ(volume_of(UnitKind::GunSpire).extent.bits, kGunSpireMask.bits);
    EXPECT_EQ(volume_of(UnitKind::GunSpire).extent.width, kGunSpireMask.width);
    EXPECT_TRUE(volume_of(UnitKind::Factory).extent.empty()) << "no mask: the drawn box, as before";
}

TEST(Volume, ThePronsAreStillDrivableBetweenAndSolidOnTheProngs) {
    const Unit one = placed(UnitKind::GunSpire, 62 * kTile, 28 * kTile);
    const std::array<Unit, 1> units{one};
    for (int ty = 26; ty < 30; ++ty) {
        EXPECT_FALSE(building_stops_units(units, 61, ty)) << "gap " << ty;
        EXPECT_FALSE(building_stops_units(units, 62, ty)) << "gap " << ty;
        EXPECT_TRUE(building_stops_units(units, 60, ty)) << "west prong " << ty;
        EXPECT_TRUE(building_stops_units(units, 63, ty)) << "east prong " << ty;
    }
}

// --- THE JUMP: speed is what it takes
// -------------------------------------------------------------

TEST(Jump, ASpeedingTankClearsALowWallItCannotDriveOver) {
    ASSERT_TRUE(open_ground());
    Sim sim = rig(UnitKind::Tank, wall(0, 40));
    const Flight flight = drive(sim, 1);
    print("a tank at full speed, jumping a low wall (top 40) on row 29", flight);
    EXPECT_TRUE(flight.ever_airborne);
    EXPECT_GT(flight.max_y, kWallY + 1.0) << "it must end up on the far side of the wall";
    EXPECT_EQ(flight.final_lift, 0.0) << "and be back on the ground";
    EXPECT_GT(sim.find(sim.units().at(0).id)->y.raw, (kWallY + 1) * kTile);
}

TEST(Jump, TheJumpPeaksAtFiftyFiveUnitsAndLandsOnTheTwentyFirstTick) {
    ASSERT_TRUE(open_ground());
    Sim sim = rig(UnitKind::Tank, {});
    const Flight flight = drive(sim, 1);
    // RE-MEASURED, G8 (the arc): the peak is the unit's OWN height, 55 above the ground it LEFT.
    // Until G8 `lift` was the whole of a jump's height and the ground's rise or fall under it was
    // the unit's too; now the height in the air is its own and `lift` is only its distance from
    // the ground under it. This strip is "open", not level: under the flight the ground rises 1.39,
    // so the old reading (peak `lift`, 55 at the code at b24e052) is 53.61 now, and the height
    // above the ground it left is the 55 the launch and gravity give.
    struct Seed42 {
        [[nodiscard]] int height(int x, int y) const noexcept {
            return world::sample(kSeed, x, y).height;
        }
    };
    const auto ground_at = [](const Frame& f) {
        return static_cast<double>(height_under(
                   Seed42{}, Fixed::from_raw(static_cast<std::int32_t>(std::lround(f.x * kTile))),
                   Fixed::from_raw(static_cast<std::int32_t>(std::lround(f.y * kTile)))))
               / kTile;
    };
    const double left = ground_at(flight.frames.front());
    double peak = 0;
    double peak_lift = 0;
    int landed = -1;
    for (const Frame& f : flight.frames) {
        peak = std::max(peak, ground_at(f) + f.lift - left);
        peak_lift = std::max(peak_lift, f.lift);
        if (landed < 0 && f.tick > 3 && f.lift == 0.0) {
            landed = f.tick;
        }
    }
    std::cout << std::format("[ G8 ] peak {:.3f} above the ground it left; {:.3f} above the ground "
                             "under it\n",
                             peak, peak_lift);
    EXPECT_NEAR(peak, 55.0, 0.001);
    EXPECT_NEAR(peak_lift, 53.612, 0.001);
    // Asked before the step that ends tick 2, so tick 2 is the first tick of flight (lift 10) and
    // the 21st, where the height is spent, is tick 22.
    EXPECT_EQ(landed, 22);
}

TEST(Jump, TheSameJumpAtATallWallIsABump) {
    ASSERT_TRUE(open_ground());
    Sim sim = rig(UnitKind::Tank, wall(0, 120));
    const Flight flight = drive(sim, 1);
    print("the same tank, the same jump, a tall wall (top 120)", flight);
    EXPECT_TRUE(flight.ever_airborne) << "it did jump";
    EXPECT_LT(flight.max_y, static_cast<double>(kWallY)) << "and was stopped at the wall";
}

TEST(Jump, BelowTheSpeedThresholdThereIsNoJumpAndTheWallIsABump) {
    ASSERT_TRUE(open_ground());
    // THE NEGATIVE CONTROL: a jump with no gate is not his rule. Same moment, same order, a unit at
    // full speed on each side of the threshold: the TANK (13107 raw a tick) is let go, the WALKER
    // (half a tank's speed, 6553, below the three quarters it takes) is refused -- and the walker
    // really is moving when it asks, so this is the speed and not a standing start.
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Walker}) {
        Sim sim = rig(kind, {});
        const UnitId id = sim.units().at(0).id;
        sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(kStartX), Fixed::from_int(kGoalY)}});
        for (int t = 0; t < 12; ++t) {
            sim.step();
        }
        const Unit& u = *sim.find(id);
        const std::int64_t speed = isqrt(static_cast<std::int64_t>(u.vx.raw) * u.vx.raw
                                         + static_cast<std::int64_t>(u.vy.raw) * u.vy.raw);
        ASSERT_EQ(speed, profile(kind).max_speed.raw) << "at its full speed";
        const std::size_t logged = sim.command_log().size();
        sim.enqueue(Order{id, orders::Jump{}});
        sim.step();
        const bool jumped = airborne(*sim.find(id));
        std::cout << std::format("[ M5 ] {} at {} raw a tick (threshold {}): jump {}\n",
                                 kind == UnitKind::Tank ? "tank  " : "walker", speed, kJumpSpeedRaw,
                                 jumped ? "TAKEN" : "refused");
        EXPECT_EQ(jumped, kind == UnitKind::Tank);
        EXPECT_EQ(sim.command_log().size() > logged, kind == UnitKind::Tank);
    }
    // ... and a TANK from a standstill has no speed either: the order lands on the very tick the
    // move does, nothing was moving, and the same low wall is a bump.
    Sim sim = rig(UnitKind::Tank, wall(0, 40));
    const Flight flight = drive(sim, 0);
    EXPECT_FALSE(flight.ever_airborne) << "a standing start is below the threshold";
    EXPECT_LT(flight.max_y, static_cast<double>(kWallY));
}

TEST(Jump, TheThresholdIsThreeQuartersOfATanksSpeedAndAWalkerIsBelowIt) {
    EXPECT_EQ(kJumpSpeedRaw, kTankSpeedRaw * 3 / 4);
    EXPECT_GE(profile(UnitKind::Tank).max_speed.raw, kJumpSpeedRaw);
    EXPECT_LT(profile(UnitKind::Walker).max_speed.raw, kJumpSpeedRaw);
    EXPECT_LT(profile(UnitKind::Turret).max_speed.raw, kJumpSpeedRaw);
}

// --- THE GROUND CASE IS UNCHANGED
// -----------------------------------------------------------------

TEST(Jump, ATankOnTheGroundStillBumpsALowWall) {
    ASSERT_TRUE(open_ground());
    Sim sim = rig(UnitKind::Tank, wall(0, 40));
    const Flight flight = drive(sim, -1);
    EXPECT_FALSE(flight.ever_airborne);
    EXPECT_LT(flight.max_y, static_cast<double>(kWallY))
        << "a wall is a wall to a unit with no height";
    EXPECT_EQ(sim.find(sim.units().at(0).id)->lift.raw, 0);
}

TEST(Jump, AnOrderThatIsRefusedLeavesTheStateExactlyAsItWas) {
    // The hash is the regression: a standing tank told to jump is refused, and a battle with the
    // refused order in it hashes as a battle without. (The pinned battles in test_observer and the
    // replay suite are the rest of this: none of them jumps, and none moved.)
    ASSERT_TRUE(open_ground());
    Sim plain = rig(UnitKind::Tank, {});
    Sim asked = rig(UnitKind::Tank, {});
    const UnitId id = plain.units().at(0).id;
    asked.enqueue(Order{id, orders::Jump{}});
    for (int t = 0; t < 10; ++t) {
        plain.step();
        asked.step();
        ASSERT_EQ(plain.state_hash(), asked.state_hash()) << "tick " << t;
    }
}

// THE HASH. A unit in the air has a height the hash must see (it decides every later step), and it
// is hashed ONLY while the unit is in the air, so no battle that never jumps moved: the pinned
// scripted battle in test_observer (final_hash 8803758361435364308) and every replay pin are the
// proof, and the whole suite passed unchanged. This is a NEW battle, so there is no OLD value to
// quote: the one below is the first recording. Causality, shown by hand on this very line: taking
// the two `h.i32(u.lift...)` lines out of `Sim::state_hash` moves the mid-flight number (it then
// agrees with a battle in which the unit never left the ground, at that tick) and restoring them
// brings it back.
TEST(Jump, AJumpBattleHashesToTheFirstRecordedValuesAndTheHeightIsPartOfThem) {
    ASSERT_TRUE(open_ground());
    Sim jumped = rig(UnitKind::Tank, wall(0, 40));
    Sim walked = rig(UnitKind::Tank, wall(0, 40));
    const UnitId id = jumped.units().at(0).id;
    jumped.enqueue(Order{id, orders::MoveTo{Fixed::from_int(kStartX), Fixed::from_int(kGoalY)}});
    walked.enqueue(Order{id, orders::MoveTo{Fixed::from_int(kStartX), Fixed::from_int(kGoalY)}});
    std::uint64_t mid_air = 0;
    for (int t = 0; t < kRunTicks; ++t) {
        if (t == 1) {
            jumped.enqueue(Order{id, orders::Jump{}});
        }
        jumped.step();
        walked.step();
        if (t == 11) {
            mid_air = jumped.state_hash();
            EXPECT_TRUE(airborne(*jumped.find(id)));
        }
    }
    EXPECT_NE(jumped.state_hash(), walked.state_hash());
    // RE-RECORDED, G1 (go around): old values 11160863739809715277 (mid-air) and
    // 15438935565811841333 (the end) are the code at 20ec749. NO BEHAVIOUR MOVED here: a unit's
    // route now carries a failure, a stall count and a goal kind in the hash, and that alone moves
    // every hash. Proved by taking those lines out of `Sim::state_hash` with the new planner left
    // in: both old values came back exactly.
    // RE-MEASURED, G6 (the press): old values 14985196521606509677 (mid-air) and
    // 5149234450444275317 (the end) are the code at 896f2e5. A HILL-TRAVEL scenario, though a
    // gentle one: "open ground" means no rough tiles, not level ones, and on column 69 the ground
    // rises from 45 to 50 between rows 28 and 32, then falls to 43 at row 34. The tank drives
    // that rise at full throttle, so its speed comes from the press now, not the old grade factor,
    // and where it stands each tick moves, and the hash with it. The `EXPECT_NE` above still holds:
    // the jump is still part of the hash. Two runs of the new values agree.
    // RE-MEASURED, G7 (gravity and friction; the drawn triangles): old values
    // 17553760078037519566 (mid-air) and 18291473291112434540 (the end) are the code at 44bcc4a,
    // restored exactly by
    // compiling G6's quad-mean ground and G6's parked drift back in (temporary toggles, removed),
    // which brought every one of this slice's six moved recordings back to its old value. THE
    // TRIANGLES move it, not the roll: with G6's parked drift compiled back in and the triangles
    // kept, both new values come out unchanged. The tank drives that rise on column 69, whose
    // tiles' corners are not in one plane, so the press it climbs against is the drawn triangle's,
    // not the quad's mean. Two runs agree.
    // RE-MEASURED, G8 (the arc; the height in the air is the unit's own): old mid-air value
    // 4795219193172317921 is the code at b24e052. THE REFERENCE FRAME moves it, and nothing else:
    // with the new code and only `settle` measuring the height against the ground the unit crossed
    // FROM (M5's frame: a temporary toggle, removed) the old value comes back exactly. The ground
    // under this flight rises (the peak test above: 1.39 units), so at tick 11 the jump's `lift`
    // -- hashed while it is in the air -- is its own height less a higher ground. It lands on the
    // same tick at the same place, so the END value is unchanged (2778783620914854318, both
    // ways). Two runs agree.
    EXPECT_EQ(mid_air, 4544634129219976427ULL);
    EXPECT_EQ(jumped.state_hash(), 2778783620914854318ULL);
}

TEST(Jump, ABuildingAndAFactoryCannotJump) {
    Sim sim = rig(UnitKind::GunSpire, {});
    const UnitId id = sim.units().at(0).id;
    sim.enqueue(Order{id, orders::Jump{}});
    sim.step();
    EXPECT_FALSE(airborne(*sim.find(id)));
}

// --- ONE GEOMETRY: a bolt reads the same volume
// ---------------------------------------------------

/// Fires one level bolt east along row 31, from tile 60 at `above` units over the ground, at a
/// tank standing at tile 70 behind a terrain wall on column 65 (top 40). Returns whether the tank
/// was hit.
[[nodiscard]] bool bolt_hits_behind_wall(int above, int lift_of_tank = 0) {
    const std::vector<PlacedVolume> wall_65{
        PlacedVolume{65, 28, false, Volume{SolidMask{1, 8, 0xFF}, units_of(0), units_of(40)}}};
    std::vector<Unit> units{placed(UnitKind::Tank, 70 * kTile + kTile / 2, 31 * kTile + kTile / 2)};
    units.front().side = Side::Enemy;
    units.front().lift = units_of(lift_of_tank);
    Projectile bolt{};
    bolt.x = Fixed::from_raw(60 * kTile);
    bolt.y = Fixed::from_raw(31 * kTile + kTile / 2);
    // Heights are measured over the ground AT THE WALL (column 65), the datum its top is above:
    // this strip is flat terrain but not level (the ground at tile 60 is 35 units, at 65 about 53).
    const Fixed wall_ground = ground_height(kSeed, Fixed::from_raw(65 * kTile + kTile / 2), bolt.y);
    bolt.z = Fixed::from_raw(wall_ground.raw + above * kTile);
    bolt.vx = Fixed::from_raw(kTile / 2);
    bolt.damage = 10;
    bolt.shooter = UnitId{99};
    bolt.side = Side::Player;
    std::vector<Projectile> flight{bolt};
    std::vector<Impact> impacts;
    std::vector<Landing> landings;
    for (int t = 0; t < 40 && !flight.empty(); ++t) {
        advance_projectiles(flight, units, impacts, landings, kSeed, nullptr, wall_65);
    }
    return !impacts.empty();
}

TEST(Volume, ABoltBelowTheTopOfATerrainWallIsStoppedAndOneAboveItFliesOver) {
    ASSERT_TRUE(open_ground());
    EXPECT_FALSE(bolt_hits_behind_wall(25)) << "at 25 over the ground the wall (top 40) stops it";
    EXPECT_TRUE(bolt_hits_behind_wall(50)) << "at 50 it goes over, into the tank's body";
}

TEST(Volume, ABoltHitsAJumpedUnitWhereItIsAndNotWhereItWouldHaveBeen) {
    ASSERT_TRUE(open_ground());
    // No wall in the way at these heights: 100 is over the wall, a tank body spans 8 .. 65.6 on the
    // ground and 63 .. 120.6 lifted by 55.
    EXPECT_FALSE(bolt_hits_behind_wall(100, 0)) << "a bolt above a grounded tank's head misses";
    EXPECT_TRUE(bolt_hits_behind_wall(100, 55)) << "the same bolt hits it when it is in the air";
}

} // namespace
} // namespace protekt::sim
