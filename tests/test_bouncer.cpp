#include "protekt/game/weapon.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/replay.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

/// M3: the bouncer round. A lob with the mortar's own ballistics that does not burst: it bounces,
/// comes to rest, spins and pulses shrapnel that hurts less the further away you are, then
/// vanishes. Every number here is read from `kBouncer` -- the spec table is the one source.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = Fixed::kOne;

struct Level {
    int x{};
    int y{};
    bool found{};
};

/// A stretch of `length` + 1 level, non-crater tiles in a row and the row beside it.
[[nodiscard]] Level find_level_row(int length) {
    constexpr int kTolerance = 6;
    for (int y = 5; y < kMapTiles - 5; ++y) {
        for (int x = 5; x + length < kMapTiles - 5; ++x) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                const world::Tile t = world::sample(kSeed, x + i, y);
                const world::Tile below = world::sample(kSeed, x + i, y + 1);
                level = t.terrain != world::Terrain::Crater && std::abs(t.height - h0) <= kTolerance
                        && std::abs(below.height - h0) <= kTolerance;
            }
            if (level) {
                return Level{x, y, true};
            }
        }
    }
    return Level{};
}

[[nodiscard]] Unit make_unit(std::uint32_t id, UnitKind kind, Side side, Fixed x, Fixed y) {
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = x;
    unit.y = y;
    unit.hp = spec_of(kind).hp;
    return unit;
}

[[nodiscard]] Fixed climb_for(double pitch) {
    return Fixed::from_raw(static_cast<std::int32_t>(std::lround(std::tan(pitch) * 65536.0)));
}

struct Flight {
    Fixed first_x{}; ///< where it first touched down
    Fixed rest_x{};  ///< where it stopped
    Fixed rest_y{};
    std::uint32_t bounces{};
    int ticks_to_rest{};
    bool rested{};
};

/// Flies a bouncer from `shooter` at `pitch` with nobody about, and reports where it touched down
/// first and where it came to rest.
[[nodiscard]] Flight fly_alone(const Unit& shooter, double pitch) {
    std::vector<Unit> units{shooter};
    std::vector<Projectile> shells{launch_lob(shooter, Fixed::from_int(1), Fixed::zero(),
                                              climb_for(pitch), UnitId{}, 0, kSeed, Round::Bounce)};
    std::vector<Impact> impacts;
    Flight out{};
    for (int tick = 1; tick < 2000 && !shells.empty(); ++tick) {
        advance_projectiles(shells, units, impacts, kSeed);
        if (shells.empty()) {
            break;
        }
        const Projectile& s = shells.front();
        if (s.bounces == 1 && out.first_x.raw == 0) {
            out.first_x = s.x;
        }
        if (s.resting && !out.rested) {
            out.rested = true;
            out.rest_x = s.x;
            out.rest_y = s.y;
            out.bounces = s.bounces;
            out.ticks_to_rest = tick;
        }
    }
    return out;
}

// --- the spec and the order -------------------------------------------------------------------

TEST(Bouncer, SpecIsPinnedAndEveryNumberIsData) {
    EXPECT_EQ(kBouncer.restitution.raw, kTile / 2);
    EXPECT_EQ(kBouncer.horizontal_keep.raw, kTile * 3 / 4);
    EXPECT_EQ(kBouncer.max_bounces, 3U);
    EXPECT_EQ(kBouncer.min_bounce_vz.raw, 6 * kTile);
    EXPECT_EQ(kBouncer.rest_ticks, 90U);
    EXPECT_EQ(kBouncer.pulse_interval, 15U);
    EXPECT_EQ(kBouncer.pulse_radius.raw, 3 * kTile);
    EXPECT_EQ(kBouncer.pulse_damage, 12U);
    EXPECT_EQ(kBouncer.rest_ticks % kBouncer.pulse_interval, 0U) << "the last pulse ends the rest";
}

TEST(Bouncer, TheOrderCarriesTheRoundAndAContactLobIsTodaysLob) {
    const orders::FireLob old_style{Fixed::from_int(1), Fixed::zero(), UnitId{3},
                                    Fixed::from_raw(9)};
    EXPECT_EQ(old_style.round, Round::Contact) << "the four-field order means Contact";
    orders::FireLob bounce = old_style;
    bounce.round = Round::Bounce;
    const OrderFields contact_fields = fields_of(OrderPayload{old_style});
    const OrderFields bounce_fields = fields_of(OrderPayload{bounce});
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(contact_fields.positions.at(i), bounce_fields.positions.at(i));
    }
    EXPECT_EQ(contact_fields.positions.at(3).raw, 0) << "Contact hashes as the unused slot did";
    EXPECT_NE(bounce_fields.positions.at(3).raw, 0);

    auto hash_after = [](const orders::FireLob& lob) {
        Sim sim = Sim::create(Scenario{kSeed, {{UnitKind::Tank, Side::Player, 40, 40}}});
        sim.enqueue(Order{sim.units()[0].id, lob});
        return sim.state_hash();
    };
    orders::FireLob explicit_contact = old_style;
    explicit_contact.round = Round::Contact;
    EXPECT_EQ(hash_after(old_style), hash_after(explicit_contact));
    EXPECT_NE(hash_after(old_style), hash_after(bounce));
}

TEST(Bouncer, TheOrderRoundTripsThroughAReplay) {
    Replay replay;
    replay.seed = kSeed;
    orders::FireLob bounce{Fixed::from_int(1), Fixed::zero(), UnitId{4}, Fixed::from_raw(500)};
    bounce.round = Round::Bounce;
    replay.commands.push_back(Command{3, Order{UnitId{2}, bounce}});
    const std::string text = to_text(replay);
    std::string reason;
    const auto back = parse(text, &reason);
    if (!back.has_value()) {
        FAIL() << reason;
    }
    ASSERT_EQ(back->commands.size(), 1U);
    const auto* lob = std::get_if<orders::FireLob>(&back->commands[0].order.payload);
    ASSERT_NE(lob, nullptr);
    EXPECT_EQ(lob->round, Round::Bounce);
}

TEST(Bouncer, ASimFiredBouncerLeavesAShellMarkedBounceAndAContactOneDoesNot) {
    for (const Round round : {Round::Contact, Round::Bounce}) {
        Sim sim = Sim::create(Scenario{kSeed, {{UnitKind::Tank, Side::Player, 40, 40}}});
        for (int i = 0; i < profile(UnitKind::Tank).cooldown_ticks; ++i) {
            sim.step();
        }
        orders::FireLob lob{Fixed::from_int(1), Fixed::zero(), UnitId{}, Fixed::from_raw(40000)};
        lob.round = round;
        sim.enqueue(Order{sim.units()[0].id, lob});
        sim.step();
        ASSERT_EQ(sim.projectiles().size(), 1U);
        EXPECT_EQ(sim.projectiles()[0].round, round);
    }
}

// --- the ballistics ---------------------------------------------------------------------------

TEST(Bouncer, ItsFirstLandingIsTheMortarLaddersLanding) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Unit gun
        = make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(at.x), Fixed::from_int(at.y));
    for (const double pitch : {0.4, 0.8, 1.0}) {
        const Flight f = fly_alone(gun, pitch);
        ASSERT_NE(f.first_x.raw, 0) << pitch;
        const double landed = static_cast<double>(f.first_x.raw - gun.x.raw) / kTile;
        EXPECT_NEAR(landed, game::mortar_landing_tiles(static_cast<float>(pitch)), 0.6)
            << "the sight must not lie, pitch " << pitch;
    }
}

TEST(Bouncer, ItBouncesOnPastTheFirstLandingAndTheCapBoundsHowFar) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Unit gun
        = make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(at.x), Fixed::from_int(at.y));
    const Flight f = fly_alone(gun, 0.8);
    ASSERT_TRUE(f.rested);
    const double first = static_cast<double>(f.first_x.raw - gun.x.raw) / kTile;
    const double rest = static_cast<double>(f.rest_x.raw - gun.x.raw) / kTile;
    EXPECT_GT(rest, first + 0.3) << "the bounce carries it further";
    EXPECT_LT(rest, first * 1.6 + 0.5) << "and damping bounds how far";
    EXPECT_LE(f.bounces, kBouncer.max_bounces);
    EXPECT_GE(f.bounces, 1U);
}

TEST(Bouncer, ASteepShotBouncesExactlyTheCap) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Unit gun
        = make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(at.x), Fixed::from_int(at.y));
    const Flight f = fly_alone(gun, 1.28);
    ASSERT_TRUE(f.rested);
    EXPECT_EQ(f.bounces, kBouncer.max_bounces);
}

TEST(Bouncer, ALimpBounceComesToRestEarlyOnTheThreshold) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Unit gun
        = make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(at.x), Fixed::from_int(at.y));
    const Flight f = fly_alone(gun, 0.0);
    ASSERT_TRUE(f.rested);
    EXPECT_LT(f.bounces, kBouncer.max_bounces) << "a flat lob has too little fall to bounce thrice";
}

// --- taking no life ---------------------------------------------------------------------------

TEST(Bouncer, ItTakesNoLifeOnEitherBounceContact) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Unit gun
        = make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(at.x), Fixed::from_int(at.y));
    const Flight dry = fly_alone(gun, 0.8);
    ASSERT_TRUE(dry.rested);
    // A victim standing where it first touches down: the shell lands ON it and must bounce.
    std::vector<Unit> units{
        gun, make_unit(2, UnitKind::Tank, Side::Enemy, dry.first_x, Fixed::from_int(at.y))};
    std::vector<Projectile> shells{launch_lob(gun, Fixed::from_int(1), Fixed::zero(),
                                              climb_for(0.8), UnitId{}, 0, kSeed, Round::Bounce)};
    std::vector<Impact> impacts;
    bool saw_bounce = false;
    while (!shells.empty() && !shells.front().resting) {
        advance_projectiles(shells, units, impacts, kSeed);
        saw_bounce = saw_bounce || (!shells.empty() && shells.front().bounces > 0);
    }
    ASSERT_FALSE(shells.empty());
    EXPECT_TRUE(saw_bounce);
    EXPECT_EQ(units[1].hp, spec_of(UnitKind::Tank).hp)
        << "it bounced off the unit, it did not burst";
    EXPECT_TRUE(impacts.empty());
}

// --- shrapnel ---------------------------------------------------------------------------------

/// A resting bouncer at (x, y) on level ground with victims; ticks it to its end and reports hp
/// after each tick of the rest.
struct Rest {
    std::vector<Unit> units;
    std::vector<Projectile> shells;
    std::vector<std::vector<std::uint16_t>> hp_by_tick;
    int rest_ticks_seen{};
    bool gone{};
};

[[nodiscard]] Rest run_rest(const Level at, std::vector<Unit> victims) {
    Rest r;
    const Unit gun
        = make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(at.x), Fixed::from_int(at.y));
    const Flight dry = fly_alone(gun, 0.8);
    r.units.push_back(gun);
    for (Unit& v : victims) {
        v.x = Fixed::from_raw(dry.rest_x.raw + v.x.raw);
        v.y = Fixed::from_raw(dry.rest_y.raw + v.y.raw);
        r.units.push_back(v);
    }
    r.shells.push_back(launch_lob(gun, Fixed::from_int(1), Fixed::zero(), climb_for(0.8), UnitId{},
                                  0, kSeed, Round::Bounce));
    std::vector<Impact> impacts;
    while (!r.shells.empty() && !r.shells.front().resting) {
        advance_projectiles(r.shells, r.units, impacts, kSeed);
    }
    for (int tick = 0; tick < 500 && !r.shells.empty(); ++tick) {
        advance_projectiles(r.shells, r.units, impacts, kSeed);
        std::vector<std::uint16_t> hps(r.units.size());
        std::transform(r.units.begin(), r.units.end(), hps.begin(),
                       [](const Unit& u) { return u.hp; });
        r.hp_by_tick.push_back(hps);
        if (!r.shells.empty()) {
            ++r.rest_ticks_seen;
        }
    }
    r.gone = r.shells.empty();
    return r;
}

TEST(Bouncer, DamageFallsOffWithDistanceInOneRunAndNothingBeyondTheRadius) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    // Beside the rest point, off the shell's path (it flies along +x): 0.6, 2.4 and 4 tiles north.
    auto victim = [&](std::uint32_t id, int dy_raw) {
        return make_unit(id, UnitKind::Tank, Side::Enemy, Fixed::zero(), Fixed::from_raw(-dy_raw));
    };
    const Rest r = run_rest(
        at, {victim(2, kTile * 6 / 10), victim(3, kTile * 12 / 5), victim(4, kTile * 4)});
    ASSERT_TRUE(r.gone);
    const auto& last = r.hp_by_tick.back();
    const int full = spec_of(UnitKind::Tank).hp;
    const int close = full - last[1];
    const int rim = full - last[2];
    EXPECT_GT(close, rim) << "the closer, the higher";
    EXPECT_GT(rim, 0) << "inside the radius still hurts";
    EXPECT_EQ(last[3], full) << "outside the radius loses nothing";
}

TEST(Bouncer, ItHurtsBothSides) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Rest r = run_rest(
        at, {make_unit(2, UnitKind::Tank, Side::Enemy, Fixed::zero(), Fixed::from_raw(-kTile)),
             make_unit(3, UnitKind::Tank, Side::Player, Fixed::zero(), Fixed::from_raw(kTile))});
    ASSERT_TRUE(r.gone);
    const int full = spec_of(UnitKind::Tank).hp;
    EXPECT_LT(r.hp_by_tick.back()[1], full);
    EXPECT_LT(r.hp_by_tick.back()[2], full) << "friendly fire is real";
    EXPECT_EQ(r.hp_by_tick.back()[1], r.hp_by_tick.back()[2]) << "equal distance, equal damage";
}

TEST(Bouncer, RestsForTheSpecDurationPulsesAtTheIntervalThenVanishes) {
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const Rest r = run_rest(
        at, {make_unit(2, UnitKind::Tank, Side::Enemy, Fixed::zero(), Fixed::from_raw(-kTile))});
    ASSERT_TRUE(r.gone);
    EXPECT_EQ(r.rest_ticks_seen, static_cast<int>(kBouncer.rest_ticks) - 1)
        << "present for rest_ticks ticks counting the one it vanishes on";
    std::vector<int> drops;
    for (std::size_t i = 0; i < r.hp_by_tick.size(); ++i) {
        const int before = i == 0 ? spec_of(UnitKind::Tank).hp : r.hp_by_tick[i - 1][1];
        if (r.hp_by_tick[i][1] < before) {
            drops.push_back(static_cast<int>(i) + 1);
        }
    }
    ASSERT_EQ(drops.size(), kBouncer.rest_ticks / kBouncer.pulse_interval);
    for (std::size_t i = 0; i < drops.size(); ++i) {
        EXPECT_EQ(drops[i], static_cast<int>((i + 1) * kBouncer.pulse_interval));
    }
    EXPECT_EQ(drops.back(), static_cast<int>(kBouncer.rest_ticks)) << "the last pulse is the end";
}

TEST(Bouncer, TwoSameSeedRunsAreByteIdentical) {
    auto run = [] {
        Sim sim = Sim::create(Scenario{
            kSeed,
            {{UnitKind::Tank, Side::Player, 60, 60}, {UnitKind::Tank, Side::Enemy, 66, 60}}});
        for (int i = 0; i < profile(UnitKind::Tank).cooldown_ticks; ++i) {
            sim.step();
        }
        orders::FireLob lob{Fixed::from_int(1), Fixed::zero(), UnitId{}, Fixed::from_raw(40000)};
        lob.round = Round::Bounce;
        sim.enqueue(Order{sim.units()[0].id, lob});
        std::vector<std::uint64_t> hashes;
        for (int i = 0; i < 300; ++i) {
            sim.step();
            hashes.push_back(sim.state_hash());
        }
        return hashes;
    };
    EXPECT_EQ(run(), run());
}

// --- the fourth weapon ------------------------------------------------------------------------

TEST(Bouncer, IsTheFourthDrivenWeaponAndQAndTheWheelWrapOverIt) {
    using game::Weapon;
    ASSERT_EQ(game::kDrivenWeapons.size(), 4U);
    EXPECT_EQ(game::kDrivenWeapons.at(3), Weapon::Bouncer);
    EXPECT_EQ(game::next_weapon(Weapon::Mortar), Weapon::Bouncer);
    EXPECT_EQ(game::next_weapon(Weapon::Bouncer), Weapon::Gun) << "Q wraps";
    EXPECT_EQ(game::previous_weapon(Weapon::Gun), Weapon::Bouncer) << "the wheel wraps";
    EXPECT_EQ(game::previous_weapon(Weapon::Bouncer), Weapon::Mortar);
    EXPECT_EQ(game::weapon_name(Weapon::Bouncer), "BOUNCER");
    const game::ReticleShape shape = game::reticle_for(Weapon::Bouncer, 0.6F);
    EXPECT_EQ(shape.kind, game::ReticleKind::Ladder);
    EXPECT_EQ(shape.rung, game::mortar_rung(0.6F)) << "its first landing is the mortar's";
}

} // namespace
} // namespace protekt::sim
