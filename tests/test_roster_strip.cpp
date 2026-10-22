// K1: the roster strip. Digits 1-9 select a KIND, not a unit: the whole group answers, from
// anywhere, wherever its units are. Keys come out of a pool, so a kind whose last unit dies gives
// its key back and a surviving kind moves down into it.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/roster.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

/// The digit `n` (1-9) as the window reports it: USB HID 30 is "1".
[[nodiscard]] InputFrame digit(int n) { return key_frame(input::kKey1 + n - 1); }

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    std::vector<UnitId> own; ///< the player's units, in spawn order
};

[[nodiscard]] std::vector<UnitId> own_ids(const sim::Sim& sim) {
    std::vector<UnitId> ids;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            ids.push_back(unit.id);
        }
    }
    return ids;
}

/// Camera at tile (40,30) looking down +Z. Own units, in spawn order: tank at (40,40), tank at
/// (44,41), a tank at (40,2) far BEHIND the camera, a mortar at (60,41), a turret at (36,41) --
/// so THREE TANKS under one key, then a mortar and a turret. One enemy tank, in front.
[[nodiscard]] Rig make_rig(std::vector<Spawn> extra = {}) {
    Rig rig{};
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 40, 40},   {UnitKind::Tank, Side::Player, 44, 41},
        {UnitKind::Tank, Side::Player, 40, 2},    {UnitKind::Mortar, Side::Player, 60, 41},
        {UnitKind::Turret, Side::Player, 36, 41}, {UnitKind::Tank, Side::Enemy, 38, 60}};
    spawns.insert(spawns.end(), extra.begin(), extra.end());
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    rig.own = own_ids(rig.sim);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.own.at(0);
    rig.options.aim = Aim{640.0F, 360.0F};
    return rig;
}

/// Whether ANY pixel of the frame, on a grid, picks `id`: the mouse could find it from here.
[[nodiscard]] bool mouse_could_find(const Rig& rig, UnitId id) {
    for (float y = 0.0F; y < 720.0F; y += 6.0F) {
        for (float x = 0.0F; x < 1280.0F; x += 6.0F) {
            if (pick(rig.sim, rig.options, x, y).unit == id) {
                return true;
            }
        }
    }
    return false;
}

void aim_at_unit(Rig& rig, UnitId unit) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
}

[[nodiscard]] std::vector<UnitId> selected(const Interaction& player) {
    const auto units = player.selection().units();
    return {units.begin(), units.end()};
}

TEST(RosterStrip, KeyOneSelectsEveryUnitOfItsKindFromWhereItCannotBeSeen) {
    Rig rig = make_rig();
    const UnitId behind = rig.own.at(2);
    ASSERT_FALSE(mouse_could_find(rig, behind))
        << "the third tank is behind the camera: the mouse must not be able to pick it";
    Interaction player{rig.own.at(0)};
    player.handle(digit(1), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0), rig.own.at(1), rig.own.at(2)}));
    EXPECT_TRUE(player.message().empty());
}

TEST(RosterStrip, TheSelectedGroupObeysFollowWithNoNewPlumbing) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(digit(1), rig.sim, rig.options); // every tank
    aim_at_unit(rig, rig.own.at(3));               // the mortar, which is NOT in the group
    player.handle(key_frame(input::kKeyF), rig.sim, rig.options);
    // Two orders, not three: the tank the player is SITTING IN is not sent anywhere, and it is
    // own.at(0). So the group order reaches the rest of the group and leaves the driven one alone.
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U) << "one order per unit of the group, less the one being driven";
    for (const sim::Order& order : orders) {
        EXPECT_TRUE(order.unit == rig.own.at(1) || order.unit == rig.own.at(2));
        EXPECT_NE(order.unit, rig.own.at(0)) << "the driven tank is not ordered";
        const auto* follow = std::get_if<sim::orders::Follow>(&order.payload);
        ASSERT_NE(follow, nullptr);
        EXPECT_EQ(follow->target, rig.own.at(3));
    }
}

TEST(RosterStrip, EveryKeySelectsItsOwnKindAndReplacesTheLast) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(digit(1), rig.sim, rig.options);
    EXPECT_EQ(selected(player).size(), 3U) << "the three tanks";
    player.handle(digit(2), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(3)})) << "the mortar alone";
    player.handle(digit(3), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(4)})) << "the turret alone";
    // Key 4 belongs to no kind: the selection is left as it was.
    player.handle(digit(4), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(4)}));
    EXPECT_FALSE(player.message().empty());
}

TEST(RosterStrip, TheStripListsOneRowPerKindWithItsCountAndTotals) {
    Rig rig = make_rig();
    const Roster roster = roster_of(rig.sim, Side::Player);
    ASSERT_EQ(roster.groups.size(), 3U);
    const std::vector<UnitKind> kinds{UnitKind::Tank, UnitKind::Mortar, UnitKind::Turret};
    for (std::size_t i = 0; i < roster.groups.size(); ++i) {
        EXPECT_EQ(roster.groups.at(i).key, static_cast<int>(i) + 1);
        EXPECT_EQ(roster.groups.at(i).kind, kinds.at(i));
        EXPECT_FALSE(kind_label(kinds.at(i)).empty());
        std::uint16_t hp = 0;
        for (const UnitId id : roster.groups.at(i).units) {
            EXPECT_EQ(rig.sim.find(id)->side, Side::Player);
            hp = static_cast<std::uint16_t>(hp + rig.sim.find(id)->hp);
        }
        EXPECT_EQ(roster.groups.at(i).hp, hp) << "the group's hit points are its units' sum";
        EXPECT_EQ(roster.groups.at(i).max_hp,
                  static_cast<std::uint16_t>(sim::profile(kinds.at(i)).hp
                                             * roster.groups.at(i).units.size()));
    }
    EXPECT_EQ(roster.groups.at(0).units,
              (std::vector<UnitId>{rig.own.at(0), rig.own.at(1), rig.own.at(2)}))
        << "one row for all three tanks, in spawn order";
    // And the enemy never appears, from either side's point of view.
    for (const RosterGroup& group : roster.groups) {
        for (const UnitId id : group.units) {
            EXPECT_EQ(rig.sim.find(id)->side, Side::Player);
        }
    }
    const Roster theirs = roster_of(rig.sim, Side::Enemy);
    ASSERT_EQ(theirs.groups.size(), 1U);
    ASSERT_EQ(theirs.groups.at(0).units.size(), 1U);
    EXPECT_EQ(rig.sim.find(theirs.groups.at(0).units.at(0))->side, Side::Enemy);
}

TEST(RosterStrip, TheLabelsAreTheShortNames) {
    EXPECT_EQ(kind_label(UnitKind::Tank), "TANK");
    EXPECT_EQ(kind_label(UnitKind::HoverTank), "HOVER");
    EXPECT_EQ(kind_label(UnitKind::Mortar), "MORTAR");
    EXPECT_EQ(kind_label(UnitKind::Rocket), "ROCKET");
    EXPECT_EQ(kind_label(UnitKind::Turret), "TURRET");
    EXPECT_EQ(kind_label(UnitKind::Factory), "FACTORY");
}

TEST(RosterStrip, TheGroupsHitPointsFollowDamage) {
    // Two enemy tanks beside the second own tank: it takes damage before anything else does.
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    const Roster first = roster_of(rig.sim, Side::Player);
    const RosterGroup* tanks = group_for_key(first, 1);
    ASSERT_NE(tanks, nullptr);
    const std::uint16_t before = tanks->hp;
    bool lower = false;
    for (int tick = 0; tick < 600 && !lower; ++tick) {
        rig.sim.step();
        const Roster now = roster_of(rig.sim, Side::Player);
        const RosterGroup* group = group_for_key(now, 1);
        ASSERT_NE(group, nullptr);
        if (group->hp < before) {
            lower = true;
            std::uint16_t sum = 0;
            for (const UnitId id : group->units) {
                sum = static_cast<std::uint16_t>(sum + rig.sim.find(id)->hp);
            }
            EXPECT_EQ(group->hp, sum);
        }
    }
    EXPECT_TRUE(lower) << "nothing was hurt, so nothing is proved";
}

TEST(RosterStrip, ThePoolReturnsASlainKindsKey) {
    // The turret spawns first and stands in front of three enemy tanks; the factory spawns second
    // and sits far behind them. So the turret takes key 1 and the factory key 2. When the turret
    // dies the factory must come DOWN into the freed key: that is what the user's rule -- "when the
    // last unit dies, the function key space gets returned to the pool" -- does to the strip, and
    // it is the one thing the rule changes about one-key-per-unit numbering.
    const std::vector<Spawn> spawns{{UnitKind::Turret, Side::Player, 44, 41},
                                    {UnitKind::Factory, Side::Player, 4, 4},
                                    {UnitKind::Tank, Side::Enemy, 45, 41},
                                    {UnitKind::Tank, Side::Enemy, 46, 42},
                                    {UnitKind::Tank, Side::Enemy, 45, 40}};
    sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    UnitId turret{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::Turret) {
            turret = unit.id;
        }
    }
    ASSERT_TRUE(turret.valid());

    const Roster before = roster_of(sim, Side::Player);
    ASSERT_EQ(before.groups.size(), 2U);
    EXPECT_EQ(before.groups.at(0).key, 1);
    EXPECT_EQ(before.groups.at(0).kind, UnitKind::Turret);
    EXPECT_EQ(before.groups.at(1).key, 2);
    EXPECT_EQ(before.groups.at(1).kind, UnitKind::Factory);

    for (int tick = 0; tick < 8000 && sim.find(turret)->alive; ++tick) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(turret)->alive) << "the turret outlived the test, so nothing is proved";

    const Roster after = roster_of(sim, Side::Player);
    ASSERT_EQ(after.groups.size(), 1U);
    EXPECT_EQ(after.groups.at(0).kind, UnitKind::Factory);
    EXPECT_EQ(after.groups.at(0).key, 1) << "the freed key was not reused";
}

TEST(RosterStrip, AFreeKeyLeavesTheSelectionAlone) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(digit(1), rig.sim, rig.options);
    ASSERT_EQ(selected(player).size(), 3U);
    player.handle(digit(8), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0), rig.own.at(1), rig.own.at(2)}));
}

TEST(RosterStrip, AFreeKeySaysSo) {
    Rig rig = make_rig(); // three kinds: keys 4-9 are free
    Interaction player{rig.own.at(0)};
    player.handle(digit(7), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_EQ(player.message(), "You have no group 7.");
    const Roster roster = roster_of(rig.sim, Side::Player);
    EXPECT_EQ(group_for_key(roster, 7), nullptr);
    EXPECT_EQ(group_for_key(roster, 3), &roster.groups.at(2));
    EXPECT_EQ(empty_key_message(7), "You have no group 7.");
}

TEST(RosterStrip, AHundredUnitsOfAKindAreStillOneRow) {
    // Twelve more tanks on top of the three: fifteen under one key, still one row, and every unit
    // the side owns is in exactly one group -- the old strip needed an "unkeyed" tally for the
    // tenth unit and up, and grouping is what removes the need for it.
    std::vector<Spawn> more;
    more.reserve(12);
    for (int i = 0; i < 12; ++i) {
        more.push_back({UnitKind::Tank, Side::Player, 20 + (3 * i), 100});
    }
    Rig rig = make_rig(more);
    ASSERT_EQ(rig.own.size(), 17U);
    const Roster roster = roster_of(rig.sim, Side::Player);
    ASSERT_EQ(roster.groups.size(), 3U) << "one row per kind, however many units";
    ASSERT_EQ(roster.groups.at(0).kind, UnitKind::Tank);
    EXPECT_EQ(roster.groups.at(0).units.size(), 15U);
    std::size_t counted = 0;
    for (const RosterGroup& group : roster.groups) {
        counted += group.units.size();
    }
    EXPECT_EQ(counted, rig.own.size()) << "every own unit is in exactly one group";
}

TEST(RosterStrip, TheStripIsDrawnOnlyWhenAskedAndDoesNotTouchTheSim) {
    Rig rig = make_rig();
    const std::uint64_t hash = rig.sim.state_hash();
    const Scene plain = build_scene(rig.sim, rig.options);
    rig.options.show_roster = true;
    const Scene with = build_scene(rig.sim, rig.options);
    EXPECT_GT(with.vertices.size(), plain.vertices.size());
    EXPECT_EQ(rig.sim.state_hash(), hash);
    rig.options.notice = "You have no group 4.";
    const Scene noted = build_scene(rig.sim, rig.options);
    EXPECT_GT(noted.vertices.size(), with.vertices.size());
}

TEST(RosterStrip, TwoSameSeedRunsAreByteIdentical) {
    const auto run = [] {
        Rig rig = make_rig();
        rig.options.show_roster = true;
        rig.options.notice = "You have no group 4.";
        Interaction player{rig.own.at(0)};
        player.handle(digit(1), rig.sim, rig.options);
        for (int i = 0; i < 30; ++i) {
            rig.sim.step();
        }
        const std::vector<UnitId> chosen = selected(player);
        return std::make_pair(chosen, build_scene(rig.sim, rig.options, chosen));
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a.first, b.first);
    ASSERT_EQ(a.second.vertices.size(), b.second.vertices.size());
    EXPECT_EQ(std::memcmp(a.second.vertices.data(), b.second.vertices.data(),
                          a.second.vertices.size() * sizeof(a.second.vertices[0])),
              0);
}

} // namespace
} // namespace protekt::game
