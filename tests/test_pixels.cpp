#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "protekt/render/pixels.hpp"

namespace {

using protekt::render::copy_pixels;
using protekt::render::swap_red_blue;
using Bytes = std::vector<std::uint8_t>;

TEST(Pixels, SwappingRedAndBlueProducesBgra) {
    Bytes px{1, 2, 3, 4, 10, 20, 30, 40};
    swap_red_blue(px);
    EXPECT_EQ(px, (Bytes{3, 2, 1, 4, 30, 20, 10, 40}));
    EXPECT_EQ(px[3], 4);
    EXPECT_EQ(px[7], 40);
}

TEST(Pixels, TheSwapIsItsOwnInverse) {
    const Bytes original{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    Bytes px = original;
    swap_red_blue(px);
    EXPECT_NE(px, original);
    swap_red_blue(px);
    EXPECT_EQ(px, original);
}

TEST(Pixels, AnRgbaFramePassesThroughUnchanged) {
    const Bytes src{1, 2, 3, 4, 5, 6, 7, 8};
    Bytes dst(src.size(), 0);
    EXPECT_EQ(copy_pixels(src, dst, false), src.size());
    EXPECT_EQ(dst, src);
}

TEST(Pixels, ABgraFrameIsCopiedSwapped) {
    const Bytes src{1, 2, 3, 4, 5, 6, 7, 8};
    Bytes dst(src.size(), 0);
    EXPECT_EQ(copy_pixels(src, dst, true), src.size());
    EXPECT_EQ(dst, (Bytes{3, 2, 1, 4, 7, 6, 5, 8}));
}

TEST(Pixels, TheSwapCoversEveryPixel) {
    Bytes px;
    for (int i = 0; i < 7; ++i) {
        px.insert(px.end(), {9, 9, 9, 255});
    }
    px.insert(px.end(), {1, 2, 3, 4}); // only the LAST pixel differs
    swap_red_blue(px);
    EXPECT_EQ(px[px.size() - 4], 3);
    EXPECT_EQ(px[px.size() - 2], 1);
    EXPECT_EQ(px[px.size() - 1], 4);
}

TEST(Pixels, ATrailingPartialPixelIsLeftAlone) {
    Bytes px{1, 2, 3, 4, 5, 6, 7};
    swap_red_blue(px);
    EXPECT_EQ(px, (Bytes{3, 2, 1, 4, 5, 6, 7}));
}

} // namespace
