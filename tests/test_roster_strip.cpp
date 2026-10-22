// N2: the roster strip. Digits 1-9 select your own units in spawn order from anywhere, the strip
// lists the living ones with their keys, a death removes a slot and never renumbers.
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

/// Camera at tile (40,30) looking down +Z. Own units: tank at (40,40), tank at (44,41), a tank at
/// (40,2) far BEHIND the camera (third in spawn order), a mortar at (60,41), a turret at (36,41).
/// One enemy tank, in front.
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

TEST(RosterStrip, DigitThreeSelectsTheThirdOwnUnitFromWhereItCannotBeSeen) {
    Rig rig = make_rig();
    const UnitId third = rig.own.at(2);
    ASSERT_FALSE(mouse_could_find(rig, third))
        << "the third unit is behind the camera: the mouse must not be able to pick it";

    Interaction player{rig.own.at(0)};
    player.handle(digit(3), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{third}));
    EXPECT_TRUE(player.message().empty());
}

TEST(RosterStrip, TheSelectedUnitObeysFollowWithNoNewPlumbing) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(digit(3), rig.sim, rig.options);
    aim_at_unit(rig, rig.own.at(1));
    player.handle(key_frame(input::kKeyF), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.own.at(2));
    const auto* follow = std::get_if<sim::orders::Follow>(&orders.at(0).payload);
    ASSERT_NE(follow, nullptr);
    EXPECT_EQ(follow->target, rig.own.at(1));
}

TEST(RosterStrip, EveryDigitSelectsItsOwnUnitAndReplacesTheLast) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    for (int n = 1; n <= static_cast<int>(rig.own.size()); ++n) {
        player.handle(digit(n), rig.sim, rig.options);
        EXPECT_EQ(selected(player),
                  (std::vector<UnitId>{rig.own.at(static_cast<std::size_t>(n - 1))}))
            << "key " << n;
    }
}

TEST(RosterStrip, TheStripListsExactlyTheLivingOwnUnitsInSpawnOrder) {
    Rig rig = make_rig();
    const Roster roster = roster_of(rig.sim, Side::Player);
    ASSERT_EQ(roster.slots.size(), rig.own.size());
    EXPECT_EQ(roster.unkeyed, 0);
    const std::vector<UnitKind> kinds{UnitKind::Tank, UnitKind::Tank, UnitKind::Tank,
                                      UnitKind::Mortar, UnitKind::Turret};
    for (std::size_t i = 0; i < roster.slots.size(); ++i) {
        EXPECT_EQ(roster.slots.at(i).key, static_cast<int>(i) + 1);
        EXPECT_EQ(roster.slots.at(i).id, rig.own.at(i));
        EXPECT_EQ(roster.slots.at(i).kind, kinds.at(i));
        EXPECT_EQ(roster.slots.at(i).hp, rig.sim.find(rig.own.at(i))->hp);
        EXPECT_EQ(roster.slots.at(i).max_hp, sim::profile(kinds.at(i)).hp);
        EXPECT_FALSE(kind_label(kinds.at(i)).empty());
    }
    // And the enemy never appears, from either side's point of view.
    for (const RosterSlot& slot : roster.slots) {
        EXPECT_EQ(rig.sim.find(slot.id)->side, Side::Player);
    }
    const Roster theirs = roster_of(rig.sim, Side::Enemy);
    ASSERT_EQ(theirs.slots.size(), 1U);
    EXPECT_EQ(rig.sim.find(theirs.slots.at(0).id)->side, Side::Enemy);
}

TEST(RosterStrip, TheLabelsAreTheShortNames) {
    EXPECT_EQ(kind_label(UnitKind::Tank), "TANK");
    EXPECT_EQ(kind_label(UnitKind::HoverTank), "HOVER");
    EXPECT_EQ(kind_label(UnitKind::Mortar), "MORTAR");
    EXPECT_EQ(kind_label(UnitKind::Rocket), "ROCKET");
    EXPECT_EQ(kind_label(UnitKind::Turret), "TURRET");
    EXPECT_EQ(kind_label(UnitKind::Factory), "FACTORY");
}

TEST(RosterStrip, HitPointsFollowDamage) {
    // Two enemy tanks beside the second own tank: it takes damage before anything else does.
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    const std::uint16_t before = roster_of(rig.sim, Side::Player).slots.at(1).hp;
    bool lower = false;
    for (int tick = 0; tick < 600 && !lower; ++tick) {
        rig.sim.step();
        for (const RosterSlot& slot : roster_of(rig.sim, Side::Player).slots) {
            if (slot.id == rig.own.at(1) && slot.hp < before) {
                lower = true;
                EXPECT_EQ(slot.hp, rig.sim.find(slot.id)->hp);
            }
        }
    }
    EXPECT_TRUE(lower) << "nothing was hurt, so nothing is proved";
}

TEST(RosterStrip, ADeadUnitVanishesItsKeyIsDeadAndNoOtherKeyMoves) {
    Rig rig = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41},
                        {UnitKind::Tank, Side::Enemy, 47, 42},
                        {UnitKind::Tank, Side::Enemy, 46, 40}});
    const std::vector<UnitId> own = rig.own;
    int dead_index = -1;
    for (int tick = 0; tick < 6000 && dead_index < 0; ++tick) {
        rig.sim.step();
        for (std::size_t i = 0; i < own.size(); ++i) {
            if (!rig.sim.find(own.at(i))->alive) {
                dead_index = static_cast<int>(i);
            }
        }
    }
    ASSERT_GE(dead_index, 0) << "nothing died, so nothing is proved";

    // Next frame: the slot is gone, the rest are where they were.
    const Roster roster = roster_of(rig.sim, Side::Player);
    std::size_t living = 0;
    for (std::size_t i = 0; i < own.size(); ++i) {
        if (!rig.sim.find(own.at(i))->alive) {
            continue;
        }
        ++living;
        bool found = false;
        for (const RosterSlot& slot : roster.slots) {
            if (slot.id == own.at(i)) {
                found = true;
                EXPECT_EQ(slot.key, static_cast<int>(i) + 1) << "unit " << i << " was renumbered";
            }
        }
        EXPECT_TRUE(found) << "a living unit is missing from the strip";
    }
    EXPECT_EQ(roster.slots.size(), living);
    for (const RosterSlot& slot : roster.slots) {
        EXPECT_NE(slot.id, own.at(static_cast<std::size_t>(dead_index)));
    }

    // Every key: living ones select their unit, the dead one selects nothing and says so.
    for (std::size_t i = 0; i < own.size(); ++i) {
        Interaction player{UnitId{}};
        const int key = static_cast<int>(i) + 1;
        player.handle(digit(key), rig.sim, rig.options);
        if (static_cast<int>(i) == dead_index) {
            EXPECT_TRUE(player.selection().empty());
            EXPECT_EQ(player.message(), dead_key_message(key));
            EXPECT_FALSE(player.message().empty());
        } else if (rig.sim.find(own.at(i))->alive) {
            EXPECT_EQ(selected(player), (std::vector<UnitId>{own.at(i)})) << "key " << key;
        }
    }
}

TEST(RosterStrip, ADeadKeyLeavesTheSelectionAlone) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(digit(2), rig.sim, rig.options);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    player.handle(digit(8), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
}

TEST(RosterStrip, AKeyForAUnitThatNeverExistedSaysSo) {
    Rig rig = make_rig(); // five own units: keys 6-9 never existed
    Interaction player{rig.own.at(0)};
    player.handle(digit(7), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_EQ(player.message(), "You have no unit 7.");
    EXPECT_EQ(unit_for_key(rig.sim, Side::Player, 7).state, KeyState::Never);
    EXPECT_EQ(dead_key_message(4), "Unit 4 is destroyed.");
    EXPECT_EQ(empty_key_message(7), "You have no unit 7.");
}

TEST(RosterStrip, ATenthUnitHasNoKeyAndIsCountedNotNumbered) {
    std::vector<Spawn> more;
    more.reserve(5);
    for (int i = 0; i < 5; ++i) {
        more.push_back({UnitKind::Tank, Side::Player, 20 + (4 * i), 100});
    }
    Rig rig = make_rig(more); // ten own units
    ASSERT_EQ(rig.own.size(), 10U);
    const Roster roster = roster_of(rig.sim, Side::Player);
    ASSERT_EQ(roster.slots.size(), 9U);
    EXPECT_EQ(roster.unkeyed, 1);
    for (const RosterSlot& slot : roster.slots) {
        EXPECT_NE(slot.id, rig.own.at(9));
    }
    for (int key = 1; key <= 9; ++key) {
        EXPECT_EQ(unit_for_key(rig.sim, Side::Player, key).id,
                  rig.own.at(static_cast<std::size_t>(key - 1)));
    }
    // Losing one of the nine does not promote the tenth into its key.
    EXPECT_EQ(unit_for_key(rig.sim, Side::Player, 10).state, KeyState::Never);
}

TEST(RosterStrip, TheStripIsDrawnOnlyWhenAskedAndDoesNotTouchTheSim) {
    Rig rig = make_rig();
    const std::uint64_t hash = rig.sim.state_hash();
    const Scene plain = build_scene(rig.sim, rig.options);
    rig.options.show_roster = true;
    const Scene with = build_scene(rig.sim, rig.options);
    EXPECT_GT(with.vertices.size(), plain.vertices.size());
    EXPECT_EQ(rig.sim.state_hash(), hash);
    rig.options.notice = "Unit 4 is destroyed.";
    const Scene noted = build_scene(rig.sim, rig.options);
    EXPECT_GT(noted.vertices.size(), with.vertices.size());
}

TEST(RosterStrip, TwoSameSeedRunsAreByteIdentical) {
    const auto run = [] {
        Rig rig = make_rig();
        rig.options.show_roster = true;
        rig.options.notice = "Unit 4 is destroyed.";
        Interaction player{rig.own.at(0)};
        player.handle(digit(3), rig.sim, rig.options);
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
