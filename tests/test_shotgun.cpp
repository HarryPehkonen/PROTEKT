#include "protekt/sim/pellets.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

/// The shotgun's pellet pattern: a fixed fan about the aim direction, with no RNG and no
/// trigonometry in it. Every number here is read from the function under test -- there is no
/// second table to drift from.
namespace protekt::sim {
namespace {

constexpr int kTile = Fixed::kOne;

[[nodiscard]] Fixed2D aim_east() noexcept { return Fixed2D{Fixed::from_int(1), Fixed::zero()}; }

[[nodiscard]] Fixed quarter_tile() noexcept { return Fixed::from_raw(kTile / 4); }

TEST(Shotgun, TheSameInputsProduceTheSamePelletsEveryTime) {
    const PelletFan a = pellet_fan(aim_east(), 5, quarter_tile());
    const PelletFan b = pellet_fan(aim_east(), 5, quarter_tile());
    ASSERT_EQ(a.count, b.count);
    for (int i = 0; i < a.count; ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        EXPECT_EQ(a.dir[at].x.raw, b.dir[at].x.raw) << "pellet " << i;
        EXPECT_EQ(a.dir[at].y.raw, b.dir[at].y.raw) << "pellet " << i;
    }
}

TEST(Shotgun, OnePelletIsTheAimDirectionItself) {
    const PelletFan fan = pellet_fan(aim_east(), 1, quarter_tile());
    ASSERT_EQ(fan.count, 1);
    EXPECT_EQ(fan.dir[0].x.raw, kTile) << "the aim, normalised";
    EXPECT_EQ(fan.dir[0].y.raw, 0);
}

TEST(Shotgun, FivePelletsAreSymmetricAboutTheAim) {
    const PelletFan fan = pellet_fan(aim_east(), 5, quarter_tile());
    ASSERT_EQ(fan.count, 5);
    EXPECT_EQ(fan.dir[0].x.raw, fan.dir[4].x.raw) << "mirror images share their x";
    EXPECT_EQ(fan.dir[0].y.raw, -fan.dir[4].y.raw) << "and negate their y";
    EXPECT_EQ(fan.dir[1].x.raw, fan.dir[3].x.raw);
    EXPECT_EQ(fan.dir[1].y.raw, -fan.dir[3].y.raw);
    EXPECT_EQ(fan.dir[2].y.raw, 0) << "the middle pellet is on the aim axis";
    EXPECT_LT(fan.dir[0].y.raw, fan.dir[1].y.raw) << "the fan opens outwards";
}

TEST(Shotgun, EveryPelletIsAboutUnitLength) {
    for (const int count : {1, 2, 3, 5, 8, kMaxPellets}) {
        const PelletFan fan = pellet_fan(aim_east(), count, quarter_tile());
        ASSERT_EQ(fan.count, count);
        for (int i = 0; i < fan.count; ++i) {
            const Fixed2D d = fan.dir[static_cast<std::size_t>(i)];
            const std::int64_t dx = d.x.raw;
            const std::int64_t dy = d.y.raw;
            const std::int64_t len = isqrt(dx * dx + dy * dy);
            EXPECT_GE(len, kTile - kTile / 50) << "count " << count << " pellet " << i;
            EXPECT_LE(len, kTile + kTile / 50) << "count " << count << " pellet " << i;
        }
    }
}

TEST(Shotgun, TheCountIsClampedToTheArray) {
    EXPECT_EQ(pellet_fan(aim_east(), kMaxPellets + 5, quarter_tile()).count, kMaxPellets);
    EXPECT_EQ(pellet_fan(aim_east(), 0, quarter_tile()).count, 1);
    EXPECT_EQ(pellet_fan(aim_east(), -3, quarter_tile()).count, 1);
    EXPECT_EQ(pellet_fan(aim_east(), 1, quarter_tile()).count, 1);
}

} // namespace
} // namespace protekt::sim
