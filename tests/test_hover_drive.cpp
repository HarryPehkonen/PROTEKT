// The hover drive model: a driven HoverTank's hull IS the view, and A/D strafe it. A driven Tank,
// and every unit nobody drives, are untouched -- the strafe term is an input that is zero
// everywhere else, and zero must be an exact identity.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/game/player.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::InputState;
using protekt::game::PlayerCommander;
namespace sim = protekt::sim;

constexpr std::uint64_t kSeed = 42;
constexpr double kStrafeFactor = 0.75;

sim::UnitId first_of(const sim::Sim& s, sim::UnitKind kind) {
    for (const auto& unit : s.units()) {
        if (unit.side == sim::Side::Player && unit.kind == kind) {
            return unit.id;
        }
    }
    return {};
}

void tick_once(sim::Sim& s, PlayerCommander& player, sim::UnitId id) {
    std::vector<sim::Order> out;
    player.think(id, sim::observe(s, sim::Side::Player), out);
    for (const auto& order : out) {
        s.enqueue(order);
    }
    s.step();
}

double raw_dx(const sim::Sim& a, const sim::Sim& b, sim::UnitId id) {
    return static_cast<double>(b.find(id)->x.raw) - a.find(id)->x.raw;
}
double raw_dy(const sim::Sim& a, const sim::Sim& b, sim::UnitId id) {
    return static_cast<double>(b.find(id)->y.raw) - a.find(id)->y.raw;
}

TEST(HoverDrive, ZeroStrafeOrderIsTheIdentity) {
    auto plain = sim::Sim::create(sim::skirmish(kSeed));
    auto zeroed = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(plain, sim::UnitKind::Tank);
    const sim::Order go{id,
                        sim::orders::MoveTo{sim::Fixed::from_int(40), sim::Fixed::from_int(40)}};
    plain.enqueue(go);
    zeroed.enqueue(go);
    for (int i = 0; i < 60; ++i) {
        zeroed.enqueue(sim::Order{id, sim::orders::Strafe{}});
        plain.step();
        zeroed.step();
        ASSERT_EQ(plain.state_hash(), zeroed.state_hash()) << "tick " << i;
    }
}

TEST(HoverDrive, StrafeMovesAtThreeQuartersSpeedAlongTheGivenDirection) {
    auto base = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(base, sim::UnitKind::HoverTank);
    auto fwd = base;
    auto side = base;
    fwd.enqueue(sim::Order{id, sim::orders::MoveTo{base.find(id)->x, sim::Fixed::from_int(60)}});
    side.enqueue(sim::Order{id, sim::orders::Strafe{sim::Fixed::from_int(1), sim::Fixed{}}});
    fwd.step();
    side.step();
    const double ahead = raw_dy(base, fwd, id);
    const double across = raw_dx(base, side, id);
    ASSERT_GT(ahead, 0.0);
    EXPECT_NEAR(across / ahead, kStrafeFactor, 0.02);
    EXPECT_EQ(raw_dy(base, side, id), 0.0) << "a strafe along +x moves nothing along y";
}

TEST(HoverDrive, StrafeNeedsNoStateBetweenTicks) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    s.enqueue(sim::Order{id, sim::orders::Strafe{sim::Fixed::from_int(1), sim::Fixed{}}});
    s.step();
    const auto after_one = s.find(id)->x.raw;
    s.step(); // no Strafe this tick: it is an input, not a stance
    EXPECT_EQ(s.find(id)->x.raw, after_one);
}

TEST(HoverDrive, DrivenHoverHeldDStrafesRightHullYawUnchanged) {
    // RE-RECORDED (SE1): the strafe eases like the throttle, so ONE tick of held D is 1/30 of full
    // strafe, not "at least half". The intent -- D strafes right, the yaw is untouched -- is kept;
    // the half-speed floor moves to after the ramp (30 ticks), below.
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    player.set_heading(0.0F); // facing +y; the right of the hull is +x
    InputState input;
    input.strafe = 1.0F;
    player.set_input(input);
    const auto before = s;
    tick_once(s, player, id);
    EXPECT_GT(raw_dx(before, s, id), 0.0);
    EXPECT_EQ(raw_dy(before, s, id), 0.0);
    EXPECT_FLOAT_EQ(player.heading(), 0.0F);
    // The first tick of the ramp: a thirtieth of 0.75x tank speed, never more.
    EXPECT_LE(raw_dx(before, s, id), kStrafeFactor * sim::kTankSpeedRaw / 30.0 + 2.0);
}

TEST(HoverDrive, DrivenHoverHeldDReachesAtLeastHalfStrafeSpeedAfterTheRamp) {
    // The half-speed floor the one-tick test used to assert, moved to where the ramp has run.
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    player.set_heading(0.0F);
    InputState input;
    input.strafe = 1.0F;
    player.set_input(input);
    // Terrain can deflect a single step, so the best step of the run is what is asserted.
    double best = 0.0;
    for (int i = 0; i < 40; ++i) {
        const auto before = s;
        tick_once(s, player, id);
        best = std::max(best, raw_dx(before, s, id));
    }
    EXPECT_LE(best, kStrafeFactor * sim::kTankSpeedRaw);
    EXPECT_GE(best, 0.5 * kStrafeFactor * sim::kTankSpeedRaw);
}

TEST(HoverDrive, DrivenHoverHeldAStrafesLeftMirrored) {
    auto right = sim::Sim::create(sim::skirmish(kSeed));
    auto left = right;
    const auto id = first_of(right, sim::UnitKind::HoverTank);
    const auto before = right;
    PlayerCommander pr;
    PlayerCommander pl;
    InputState d;
    d.strafe = 1.0F;
    InputState a;
    a.strafe = -1.0F;
    pr.set_input(d);
    pl.set_input(a);
    tick_once(right, pr, id);
    tick_once(left, pl, id);
    EXPECT_GT(raw_dx(before, right, id), 0.0);
    EXPECT_LT(raw_dx(before, left, id), 0.0);
    EXPECT_FLOAT_EQ(pl.heading(), 0.0F);
}

TEST(HoverDrive, StrafeIsBodyRelative) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    player.set_heading(1.5707964F); // facing +x; the right of the hull is -y
    InputState input;
    input.strafe = 1.0F;
    player.set_input(input);
    const auto before = s;
    tick_once(s, player, id);
    EXPECT_LT(raw_dy(before, s, id), 0.0);
}

TEST(HoverDrive, HullYawFollowsTheViewExactlyEveryTick) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    for (int i = 0; i < 5; ++i) {
        InputState input;
        input.look_yaw = 0.3F;
        input.turn = 1.0F; // the old turn key must not turn a driven hover
        player.set_input(input);
        tick_once(s, player, id);
        EXPECT_EQ(player.heading(), player.view_yaw()) << "tick " << i;
        EXPECT_EQ(player.turret_yaw(), 0.0F);
    }
}

TEST(HoverDrive, DrivenTankIsUntouched) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::Tank);
    PlayerCommander player;
    InputState input;
    input.look_yaw = 0.3F;
    input.turn = 1.0F;
    input.strafe = 1.0F; // a tank has no strafe
    player.set_input(input);
    std::vector<sim::Order> out;
    player.think(id, sim::observe(s, sim::Side::Player), out);
    EXPECT_NEAR(player.turret_yaw(), 0.3F, 1e-6F);
    EXPECT_NE(player.heading(), player.view_yaw());
    EXPECT_GT(player.turn_rate(), 0.0F) << "A/D still turn a tank";
    for (const auto& order : out) {
        EXPECT_EQ(order.kind(), sim::OrderKind::MoveTo) << "no Strafe order for a tank";
    }
}

TEST(HoverDrive, UndrivenHoverMovesLikeATank) {
    auto hover = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(hover, sim::UnitKind::HoverTank);
    hover.enqueue(
        sim::Order{id, sim::orders::MoveTo{sim::Fixed::from_int(30), sim::Fixed::from_int(30)}});
    const auto before = hover;
    hover.step();
    EXPECT_NE(hover.state_hash(), before.state_hash());
    const double step = std::hypot(raw_dx(before, hover, id), raw_dy(before, hover, id));
    EXPECT_GT(step, 0.0);
    EXPECT_LE(step, static_cast<double>(sim::kTankSpeedRaw)) << "tank speed, no strafe bonus";
}

TEST(HoverDrive, StrafeDriveIsDeterministic) {
    const auto run = [] {
        auto s = sim::Sim::create(sim::skirmish(kSeed));
        const auto id = first_of(s, sim::UnitKind::HoverTank);
        PlayerCommander player;
        for (int i = 0; i < 90; ++i) {
            InputState input;
            input.strafe = (i / 15) % 2 == 0 ? 1.0F : -1.0F;
            input.throttle = i % 40 < 20 ? 1.0F : 0.0F;
            input.look_yaw = 0.01F;
            player.set_input(input);
            tick_once(s, player, id);
        }
        return s.state_hash();
    };
    EXPECT_EQ(run(), run());
}

// ---- SE1: the sim scales a strafe by the vector's length; the player eases that length ----------

/// The length of the Strafe order in `out`, in tiles (1.0 = a full-length vector); 0 if none.
double strafe_magnitude(const std::vector<sim::Order>& out) {
    for (const auto& order : out) {
        if (const auto* o = std::get_if<sim::orders::Strafe>(&order.payload)) {
            const double dx = static_cast<double>(o->dx.raw) / sim::Fixed::kOne;
            const double dy = static_cast<double>(o->dy.raw) / sim::Fixed::kOne;
            return std::hypot(dx, dy);
        }
    }
    return 0.0;
}

/// One tick for the commander AND the sim; returns the emitted strafe length.
double tick_magnitude(sim::Sim& s, PlayerCommander& player, sim::UnitId id) {
    std::vector<sim::Order> out;
    player.think(id, sim::observe(s, sim::Side::Player), out);
    for (const auto& order : out) {
        s.enqueue(order);
    }
    s.step();
    return strafe_magnitude(out);
}

TEST(HoverDrive, SimScalesStrafeSpeedByVectorLengthAndFullLengthIsUnchanged) {
    auto base = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(base, sim::UnitKind::HoverTank);
    auto full = base;
    auto quarter = base;
    auto over = base;
    full.enqueue(sim::Order{id, sim::orders::Strafe{sim::Fixed::from_int(1), sim::Fixed{}}});
    quarter.enqueue(sim::Order{
        id, sim::orders::Strafe{sim::Fixed::from_raw(sim::Fixed::kOne / 4), sim::Fixed{}}});
    over.enqueue(sim::Order{id, sim::orders::Strafe{sim::Fixed::from_int(3), sim::Fixed{}}});
    full.step();
    quarter.step();
    over.step();
    const double f = raw_dx(base, full, id);
    const double q = raw_dx(base, quarter, id);
    ASSERT_GT(f, 0.0);
    EXPECT_NEAR(q * 4.0, f, 4.0) << "a quarter-length vector moves at a quarter of the speed";
    EXPECT_EQ(full.state_hash(), over.state_hash())
        << "length is capped at one: longer is no faster";
    // Today's number, exactly: 3/4 of the unit's speed when the ground is no slower.
    EXPECT_LE(f, kStrafeFactor * sim::kTankSpeedRaw);
}

TEST(HoverDrive, StrafeAndThrottleRampToFullOverTheSameThirtyTicks) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    InputState input;
    input.strafe = 1.0F;
    input.throttle = 1.0F;
    player.set_input(input);
    double last = 0.0;
    int strafe_full_at = 0;
    int throttle_full_at = 0;
    for (int tick = 1; tick <= 40; ++tick) {
        const double mag = tick_magnitude(s, player, id);
        EXPECT_GE(mag, last) << "monotone, tick " << tick;
        last = mag;
        if (strafe_full_at == 0 && mag >= 0.999) {
            strafe_full_at = tick;
        }
        if (throttle_full_at == 0 && player.throttle_rate() >= 0.999F) {
            throttle_full_at = tick;
        }
    }
    EXPECT_EQ(strafe_full_at, 30);
    EXPECT_EQ(throttle_full_at, 30);
    EXPECT_EQ(strafe_full_at, throttle_full_at) << "the strafe is as quick as the throttle";
    EXPECT_NEAR(last, 1.0, 0.001);
}

TEST(HoverDrive, ReleasingStrafeRampsBackToZeroNeverSnapping) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    InputState input;
    input.strafe = 1.0F;
    player.set_input(input);
    double last = 0.0;
    for (int tick = 0; tick < 30; ++tick) {
        last = tick_magnitude(s, player, id);
    }
    ASSERT_NEAR(last, 1.0, 0.001);
    player.set_input(InputState{});
    int zero_at = 0;
    for (int tick = 1; tick <= 40; ++tick) {
        const double mag = tick_magnitude(s, player, id);
        EXPECT_LE(mag, last + 1e-9) << "monotone down, tick " << tick;
        EXPECT_LE(last - mag, 1.0 / 30.0 + 0.002) << "no snap, tick " << tick;
        last = mag;
        if (zero_at == 0 && mag == 0.0) {
            zero_at = tick;
        }
    }
    EXPECT_EQ(zero_at, 30) << "back to zero over the same thirty ticks";
}

TEST(HoverDrive, OneTickStrafeTapLeavesASmallValueNotAFullSpeedLurch) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::HoverTank);
    PlayerCommander player;
    InputState input;
    input.strafe = 1.0F;
    player.set_input(input);
    const double tap = tick_magnitude(s, player, id);
    EXPECT_GT(tap, 0.0);
    EXPECT_LE(tap, 1.0 / 30.0 + 0.001);
    // A longer tap decays after release rather than stopping dead.
    InputState held = input;
    player.set_input(held);
    double peak = tap;
    for (int i = 0; i < 4; ++i) {
        peak = tick_magnitude(s, player, id);
    }
    player.set_input(InputState{});
    const double after = tick_magnitude(s, player, id);
    EXPECT_GT(after, 0.0);
    EXPECT_LT(after, peak);
}

TEST(HoverDrive, StrafeRampIsSymmetricLeftAndRight) {
    auto rs = sim::Sim::create(sim::skirmish(kSeed));
    auto ls = rs;
    const auto id = first_of(rs, sim::UnitKind::HoverTank);
    PlayerCommander pr;
    PlayerCommander pl;
    InputState d;
    d.strafe = 1.0F;
    InputState a;
    a.strafe = -1.0F;
    pr.set_input(d);
    pl.set_input(a);
    for (int tick = 0; tick < 35; ++tick) {
        std::vector<sim::Order> r;
        std::vector<sim::Order> l;
        pr.think(id, sim::observe(rs, sim::Side::Player), r);
        pl.think(id, sim::observe(ls, sim::Side::Player), l);
        for (const auto& o : r) {
            rs.enqueue(o);
        }
        for (const auto& o : l) {
            ls.enqueue(o);
        }
        rs.step();
        ls.step();
        EXPECT_NEAR(strafe_magnitude(r), strafe_magnitude(l), 1e-6) << "tick " << tick;
        EXPECT_FLOAT_EQ(pr.heading(), pl.heading());
    }
}

TEST(HoverDrive, DrivenTankStillTurnsAndNeverStrafes) {
    auto s = sim::Sim::create(sim::skirmish(kSeed));
    const auto id = first_of(s, sim::UnitKind::Tank);
    PlayerCommander player;
    InputState input;
    input.turn = 1.0F;
    input.strafe = 1.0F;
    player.set_input(input);
    for (int tick = 0; tick < 10; ++tick) {
        std::vector<sim::Order> out;
        player.think(id, sim::observe(s, sim::Side::Player), out);
        for (const auto& order : out) {
            EXPECT_NE(order.kind(), sim::OrderKind::Strafe);
            s.enqueue(order);
        }
        s.step();
    }
    EXPECT_GT(player.turn_rate(), 0.3F);
    EXPECT_NE(player.heading(), 0.0F);
    EXPECT_EQ(player.strafe_rate(), 0.0F);
}

} // namespace
