#include "protekt/game/navigate.hpp"

#include <gtest/gtest.h>

#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace protekt::game {
namespace {} // namespace

TEST(Navigate, AClearRunStopsAtACrater) {
    // Find a crater, stand two tiles short of it, and look straight at it.
    int cx = -1;
    int cy = -1;
    for (int y = 40; y < 120 && cx < 0; ++y) {
        for (int x = 40; x < 120; ++x) {
            if (world::sample(42, x, y).terrain == world::Terrain::Crater) {
                cx = x;
                cy = y;
                break;
            }
        }
    }
    ASSERT_GE(cx, 0) << "seed 42 has no craters, which is itself a surprise";
    // Heading 0 is +y, so standing 4 tiles below the crater and looking up at it must stop short.
    const int run = clear_run(42, cx, cy - 4, 0.0F);
    EXPECT_LT(run, 4) << "a crater four tiles ahead must end the run";
}

TEST(Navigate, ARunIsCappedAtWhatWasAskedFor) {
    // Not every tile is open -- the first version of this test assumed the middle of the map was,
    // and tile (128,128) turned out to be ringed with craters. So find a tile with real ground
    // ahead of it and check the two things that must hold everywhere: the cap is respected, and the
    // run is never longer than the cap.
    int open_x = -1;
    int open_y = -1;
    for (int y = 20; y < 240 && open_x < 0; y += 7) {
        for (int x = 20; x < 240; x += 7) {
            if (clear_run(42, x, y, clearest_heading(42, x, y)) == kClearRunTiles) {
                open_x = x;
                open_y = y;
                break;
            }
        }
    }
    if (open_x < 0) {
        GTEST_SKIP() << "seed 42 has no tile with a full clear run";
    }
    const float heading = clearest_heading(42, open_x, open_y);
    EXPECT_EQ(clear_run(42, open_x, open_y, heading), kClearRunTiles);
    EXPECT_EQ(clear_run(42, open_x, open_y, heading, 4), 4) << "the cap must be obeyed";
    EXPECT_LE(clear_run(42, open_x, open_y, heading, 1000), 1000);
}

TEST(Navigate, TheSpawnTankIsPointedSomewhereItCanActuallyDrive) {
    // The measured bug this exists for: at seed 42 the player's tank spawns facing four crater
    // tiles. Heading 0 must therefore have a poor run, and the chosen heading a much better one, or
    // the player's first keypress does nothing and the game looks broken.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    sim::UnitId tank{};
    for (const auto& unit : sim.units()) {
        if (unit.side == sim::Side::Player && unit.kind == sim::UnitKind::Tank) {
            tank = unit.id;
            break;
        }
    }
    ASSERT_TRUE(tank.valid());
    const sim::Unit* self = sim.find(tank);
    const int x = self->x.floor_int();
    const int y = self->y.floor_int();

    const int straight_ahead = clear_run(42, x, y, 0.0F);
    const float chosen = clearest_heading(42, x, y);
    const int chosen_run = clear_run(42, x, y, chosen);

    EXPECT_LT(straight_ahead, 2) << "the spawn wall is supposed to be there; if it is gone, this "
                                    "test is measuring nothing";
    EXPECT_GE(chosen_run, 6) << "the spawn heading must have real ground ahead of it";
    EXPECT_GT(chosen_run, straight_ahead);
}

TEST(Navigate, ChoosingAHeadingIsDeterministic) {
    for (int i = 0; i < 40; ++i) {
        const int x = 20 + (i * 7) % 200;
        const int y = 20 + (i * 11) % 200;
        EXPECT_EQ(clearest_heading(42, x, y), clearest_heading(42, x, y));
    }
}

TEST(Navigate, EveryHeadingIsTriedIncludingTheOnesBehindYou) {
    // A tank in a corridor open only to the south must be pointed south: a search that only looked
    // forward would leave it staring at a wall.
    int found_x = -1;
    int found_y = -1;
    for (int y = 60; y < 180 && found_x < 0; ++y) {
        for (int x = 60; x < 180; ++x) {
            if (clear_run(42, x, y, 0.0F) < 2 && clear_run(42, x, y, 3.14159265F) >= 6) {
                found_x = x;
                found_y = y;
                break;
            }
        }
    }
    if (found_x < 0) {
        GTEST_SKIP() << "seed 42 has no tile walled to the north and open to the south";
    }
    const float chosen = clearest_heading(42, found_x, found_y);
    EXPECT_GE(clear_run(42, found_x, found_y, chosen), 6);
}

} // namespace protekt::game
