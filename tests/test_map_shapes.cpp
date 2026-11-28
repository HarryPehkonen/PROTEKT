// SLICE G15, SHAPES IN THE MAP. Harri: "The base cuboid blocks the inner shape, and the floating
// cuboid blocks the shape that's up higher ... It would be nice if those shapes could be created in
// the map as well", and his V:
//
//     ######
//     .####.
//     ..##..
//
// A `[[volume]]` already had a mask, a base and a top, and the sections are an ARRAY. What G15
// adds: a DIGIT in the mask is that tile's own height (`app::digit_top`), and every volume is DRAWN
// as the shape it is -- a column per tile, from its base to its top, so a floating one floats.
//
// These pin: (1) several volumes per map reach the sim; (2) a digit mask loads to the per-tile tops
// its digits say; (3) the drawn shape covers the blocked shape, asked of `Sim::volumes()`, not of a
// list; (4) a `#`/`.` map loads EXACTLY as before.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include "protekt/app/map_file.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/blocked_ground.hpp"
#include "protekt/game/png.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/ground.hpp"

namespace protekt {
namespace {

using Tile = std::pair<int, int>;
using Tiles = std::set<Tile>;
constexpr std::int64_t kOne = sim::Fixed::kOne;

/// A `[[volume]]` section, as a person writes one.
[[nodiscard]] std::string volume_text(int x, int y, int base, int top,
                                      const std::vector<std::string>& rows) {
    std::string text = "\n[[volume]]\nx = " + std::to_string(x) + "\ny = " + std::to_string(y)
                       + "\nturned = false\nbase = " + std::to_string(base)
                       + "\ntop = " + std::to_string(top) + "\nmask = \"\"\"\n";
    for (const std::string& row : rows) {
        text += row + "\n";
    }
    return text + "\"\"\"\n";
}

[[nodiscard]] app::MapDoc parsed(const std::string& text) {
    const app::MapParse p = app::parse_map(text);
    EXPECT_TRUE(p.ok) << p.error;
    return p.map;
}

[[nodiscard]] app::MapDoc proving_ground() {
    const app::MapParse p
        = app::load_map_file(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    EXPECT_TRUE(p.ok) << p.error;
    return p.map;
}

[[nodiscard]] sim::Sim battle_on(const app::MapDoc& map) {
    return sim::Sim::create(
        sim::skirmish(map.seed, app::map_scenario_data(map, sim::default_data())));
}

/// The TOP of the volume on world tile (`x`, `y`), raw, if any volume is solid there; and how many
/// are (a digit mask is laid down as one volume per digit, so a tile is in at most one of them).
struct TileTop {
    std::optional<std::int64_t> top{};
    std::optional<std::int64_t> base{};
    int covering{};
};
[[nodiscard]] TileTop top_at(const std::vector<sim::PlacedVolume>& volumes, int x, int y) {
    TileTop out;
    for (const sim::PlacedVolume& v : volumes) {
        if (sim::placed_solid_at(v, x, y)) {
            ++out.covering;
            out.top = v.volume.top.raw;
            out.base = v.volume.base.raw;
        }
    }
    return out;
}

/// The tiles a GROUNDED unit is refused by a volume, the mover's own question (`placed_solid_at`
/// and `volume_stops` at lift zero, `WorldGround::factor`), asked for the tallest vehicle
/// (`tallest_body`, G16) as the hatch is.
[[nodiscard]] Tiles stops_a_grounded_unit(const std::vector<sim::PlacedVolume>& volumes, int w,
                                          int h) {
    Tiles out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (const sim::PlacedVolume& v : volumes) {
                if (sim::placed_solid_at(v, x, y)
                    && sim::volume_stops(v.volume, sim::Fixed::zero(), sim::tallest_body())) {
                    out.insert({x, y});
                }
            }
        }
    }
    return out;
}

[[nodiscard]] std::string listed(const Tiles& tiles) {
    std::ostringstream out;
    for (const auto& [x, y] : tiles) {
        out << " (" << x << "," << y << ")";
    }
    return out.str();
}

// ---- (1) MANY VOLUMES PER MAP ---------------------------------------------------------------

TEST(MapShapes, ThreeVolumesAtThreeHeightsAllReachTheSim) {
    // The sections are an ARRAY: a wall on the ground, a slab a tank fits under, and one higher
    // still. Nothing new was needed for this; it pins that it stays so.
    std::string text = app::write_map(app::generate_map(3, 24));
    text += volume_text(2, 2, 0, 40, {"##"});
    text += volume_text(6, 2, 80, 120, {"###"});
    text += volume_text(12, 2, 200, 260, {"#"});
    const app::MapDoc map = parsed(text);
    ASSERT_EQ(map.volumes.size(), 3U);
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    const std::vector<sim::PlacedVolume>& v = sim.volumes();
    ASSERT_EQ(v.size(), 3U) << "every [[volume]] is in Scenario::volumes";
    const std::vector<std::pair<int, int>> heights{{0, 40}, {80, 120}, {200, 260}};
    const std::vector<int> xs{2, 6, 12};
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(v.at(i).x, xs.at(i));
        EXPECT_EQ(v.at(i).y, 2);
        EXPECT_EQ(v.at(i).volume.base.raw, heights.at(i).first * kOne) << i;
        EXPECT_EQ(v.at(i).volume.top.raw, heights.at(i).second * kOne) << i;
    }
}

// ---- (2) A DIGIT MASK -----------------------------------------------------------------------

TEST(MapShapes, ADigitMaskLoadsToTheTopsItsDigitsSay) {
    // THE RULE (`app::digit_top`): digit d on a tile is a blockage from `base` to
    // base + d * (top - base) / 9 there; '.' and '0' are no blockage; '#' is the whole `top`.
    // base 0, top 90: each digit is 10 units of height, so the expected tops are exact.
    std::string text = app::write_map(app::generate_map(3, 24));
    text += volume_text(2, 2, 0, 90, {"123.#0", "456789"});
    // base 10, top 50: 40 / 9 is not whole. digit 1 tops out at 10 + 40/9 = 14.444... units,
    // raw 10 * 65536 + (1 * 40 * 65536) / 9 = 655360 + 291271 = 946631 (the division floors).
    text += volume_text(2, 6, 10, 50, {"1#5"});
    const app::MapDoc map = parsed(text);
    const std::vector<sim::PlacedVolume> placed = app::placed_volumes(map);

    // Row 0, "123.#0", and row 1, "456789", of the first; expected top in whole units, or none.
    const std::vector<std::vector<std::optional<int>>> expected{
        {10, 20, 30, std::nullopt, 90, std::nullopt}, {40, 50, 60, 70, 80, 90}};
    for (int ly = 0; ly < 2; ++ly) {
        for (int lx = 0; lx < 6; ++lx) {
            const TileTop t = top_at(placed, 2 + lx, 2 + ly);
            const std::optional<int>& want
                = expected.at(static_cast<std::size_t>(ly)).at(static_cast<std::size_t>(lx));
            EXPECT_LE(t.covering, 1) << lx << "," << ly << ": one volume per tile";
            if (!want.has_value()) {
                EXPECT_EQ(t.covering, 0) << lx << "," << ly << " is open";
                continue;
            }
            ASSERT_EQ(t.covering, 1) << lx << "," << ly << " is solid";
            EXPECT_EQ(t.top, std::optional<std::int64_t>{*want * kOne}) << lx << "," << ly;
            EXPECT_EQ(t.base, std::optional<std::int64_t>{0}) << lx << "," << ly;
        }
    }
    // The second: 1, #, 5 from base 10.
    EXPECT_EQ(top_at(placed, 2, 6).top, std::optional<std::int64_t>{946631});
    EXPECT_EQ(top_at(placed, 3, 6).top, std::optional<std::int64_t>{50 * kOne}) << "# is the top";
    // digit 5: 10 * 65536 + (5 * 40 * 65536) / 9 = 655360 + 1456355 = 2111715.
    EXPECT_EQ(top_at(placed, 4, 6).top, std::optional<std::int64_t>{2111715});
    EXPECT_EQ(top_at(placed, 4, 6).base, std::optional<std::int64_t>{10 * kOne});

    // A digit mask writes and reads back as the same document.
    const app::MapDoc back = parsed(app::write_map(map));
    EXPECT_TRUE(back == map);
    EXPECT_EQ(back.volumes, map.volumes);
}

TEST(MapShapes, HisVBlocksTheVAndNothingElse) {
    // HIS ACCEPTANCE: the V, written his way with full-height digits, refuses a grounded unit on
    // exactly its tiles.
    std::string text = app::write_map(app::generate_map(3, 24));
    text += volume_text(4, 4, 0, 64, {"999999", ".9999.", "..99.."});
    const app::MapDoc map = parsed(text);
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    Tiles v;
    for (int x = 4; x < 10; ++x) {
        v.insert({x, 4});
    }
    for (int x = 5; x < 9; ++x) {
        v.insert({x, 5});
    }
    v.insert({6, 6});
    v.insert({7, 6});
    EXPECT_EQ(stops_a_grounded_unit(sim.volumes(), map.width, map.height), v);
}

TEST(MapShapes, AMaskLetterThatIsNotADigitHashOrDotIsRefused) {
    std::string text = app::write_map(app::generate_map(3, 24));
    text += volume_text(2, 2, 0, 90, {"12x"});
    const app::MapParse p = app::parse_map(text);
    EXPECT_FALSE(p.ok);
    EXPECT_NE(p.error.find("0-9"), std::string::npos) << p.error;
}

// ---- (3) THE DRAWN SHAPE COVERS THE BLOCKED SHAPE --------------------------------------------

/// What the scene draws of the map's volumes: on each world tile, the lowest underside and the
/// highest top of the triangles in the volume colour (`game::kVolumeColour`). A top face's centroid
/// is inside its tile; a side face's is on the edge, so only faces that look up or down are read.
struct DrawnColumn {
    float bottom{std::numeric_limits<float>::max()};
    float top{std::numeric_limits<float>::lowest()};
};
[[nodiscard]] std::map<Tile, DrawnColumn> drawn_volumes(const game::Scene& scene) {
    const auto is_volume
        = [](const game::SceneVertex& v) { return v.colour == game::kVolumeColour; };
    std::map<Tile, DrawnColumn> out;
    for (std::size_t i = 0; i + 2 < scene.vertices.size(); i += 3) {
        const game::SceneVertex& a = scene.vertices.at(i);
        const game::SceneVertex& b = scene.vertices.at(i + 1);
        const game::SceneVertex& c = scene.vertices.at(i + 2);
        if (!is_volume(a) || !is_volume(b) || !is_volume(c)) {
            continue;
        }
        const float ny = a.normal[1];
        if (std::abs(ny) < 0.5F) {
            continue;
        }
        const float x = (a.position[0] + b.position[0] + c.position[0]) / 3.0F;
        const float z = (a.position[2] + b.position[2] + c.position[2]) / 3.0F;
        const Tile tile{static_cast<int>(std::floor(x / game::kMetresPerTile)),
                        static_cast<int>(std::floor(z / game::kMetresPerTile))};
        DrawnColumn& col = out[tile];
        for (const game::SceneVertex* v : {&a, &b, &c}) {
            if (ny > 0.0F) {
                col.top = std::max(col.top, v->position[1]);
            } else {
                col.bottom = std::min(col.bottom, v->position[1]);
            }
        }
    }
    return out;
}

[[nodiscard]] game::SceneOptions looking_at(std::uint64_t seed, int x, int y, int radius) {
    game::SceneOptions o;
    o.seed = seed;
    o.radius_tiles = radius;
    o.camera.x = (static_cast<float>(x) + 0.5F) * game::kMetresPerTile;
    o.camera.z = (static_cast<float>(y) + 0.5F) * game::kMetresPerTile;
    o.camera.y = 8.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

/// The ground under the middle of tile (`x`, `y`) in metres: the datum a volume's heights are
/// measured from (the bolt's own, `projectile.cpp`).
[[nodiscard]] float datum_metres(std::uint64_t seed, int x, int y) {
    const auto mid = [](int t) {
        return sim::Fixed::from_raw(static_cast<std::int32_t>(t * kOne + kOne / 2));
    };
    return static_cast<float>(sim::ground_height(seed, mid(x), mid(y)).raw)
           / static_cast<float>(kOne) * game::kMetresPerHeightUnit;
}

[[nodiscard]] float metres(sim::Fixed units) {
    return static_cast<float>(units.raw) / static_cast<float>(kOne) * game::kMetresPerHeightUnit;
}

/// THE GUARD AGAINST DRIFT (G14's for buildings, here for terrain): every tile of every volume the
/// sim holds is drawn, as a column that reaches at least from the volume's base to its top there;
/// a floating one's underside is at its base, not on the ground; and nothing is drawn as a volume
/// where no volume is.
void expect_drawn_covers_blocked(const app::MapDoc& map, int cx, int cy, int radius) {
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    ASSERT_FALSE(sim.volumes().empty());
    const game::Scene scene = game::build_scene(sim, looking_at(map.seed, cx, cy, radius));
    const std::map<Tile, DrawnColumn> drawn = drawn_volumes(scene);
    constexpr float kTol = 1e-3F;
    Tiles missing;
    Tiles short_top;
    Tiles low_bottom;
    Tiles high_bottom;
    int floating_tiles = 0;
    for (const sim::PlacedVolume& v : sim.volumes()) {
        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                if (!sim::placed_solid_at(v, x, y)) {
                    continue;
                }
                const auto it = drawn.find({x, y});
                if (it == drawn.end()) {
                    missing.insert({x, y});
                    continue;
                }
                const float datum = datum_metres(map.seed, x, y);
                if (it->second.top < datum + metres(v.volume.top) - kTol) {
                    short_top.insert({x, y});
                }
                if (it->second.bottom > datum + metres(v.volume.base) + kTol) {
                    high_bottom.insert({x, y});
                }
                if (v.volume.base.raw > 0) {
                    ++floating_tiles;
                    // READS AS FLOATING: the underside is at the base, with open air under it.
                    if (it->second.bottom < datum + metres(v.volume.base) - kTol) {
                        low_bottom.insert({x, y});
                    }
                }
            }
        }
    }
    EXPECT_TRUE(missing.empty()) << "volume tiles with nothing drawn (an INVISIBLE WALL):"
                                 << listed(missing);
    EXPECT_TRUE(short_top.empty()) << "drawn lower than the volume's top:" << listed(short_top);
    EXPECT_TRUE(high_bottom.empty())
        << "drawn starting above the volume's base:" << listed(high_bottom);
    EXPECT_TRUE(low_bottom.empty())
        << "a floating volume drawn down to the ground:" << listed(low_bottom);
    for (const auto& [tile, col] : drawn) {
        EXPECT_GT(top_at(sim.volumes(), tile.first, tile.second).covering, 0)
            << tile.first << "," << tile.second << " is drawn as a volume and is none";
    }
    // The ground under a volume is hatched exactly where a grounded unit is refused: a slab he can
    // drive under is NOT painted as a wall under him.
    for (const sim::PlacedVolume& v : sim.volumes()) {
        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                if (!sim::placed_solid_at(v, x, y)) {
                    continue;
                }
                const bool stops
                    = sim::volume_stops(v.volume, sim::Fixed::zero(), sim::tallest_body());
                EXPECT_EQ(game::ground_blocked(sim.volumes(), x, y),
                          stops || world::ground_solid(x, y))
                    << x << "," << y;
                EXPECT_EQ(game::ground_overhead(sim.volumes(), x, y),
                          !stops && !world::ground_solid(x, y))
                    << x << "," << y << ": the shade under a slab a unit drives under";
            }
        }
    }
    (void)floating_tiles;
}

TEST(MapShapes, TheDrawnShapeCoversTheBlockedShape) {
    // His V, a taper, a slab a tank fits under, and a slab too low to: every volume, drawn.
    std::string text = app::write_map(app::generate_map(3, 24));
    text += volume_text(2, 2, 0, 64, {"999999", ".9999.", "..99.."});
    text += volume_text(2, 8, 0, 90, {"12345", "6789."});
    text += volume_text(2, 12, 80, 120, {"####", "####"});
    text += volume_text(2, 16, 30, 60, {"###"});
    expect_drawn_covers_blocked(parsed(text), 12, 12, 20);
}

TEST(MapShapes, TheProvingGroundsVolumesAreDrawnAsWellAsHatched) {
    const app::MapDoc map = proving_ground();
    expect_drawn_covers_blocked(map, map.width / 2, map.height / 2, map.width / 2 + 8);
}

TEST(MapShapes, AFloatingSlabIsDrawnFloating) {
    // His word: a slab he can drive under must READ as one. Its underside is in the air (at its
    // base), and the ground under it is not the wall hatch.
    std::string text = app::write_map(app::generate_map(3, 24));
    // G16: the base was 80, "a tank fits under"; the tallest vehicle (the recycler, 2.5 m, its head
    // at 8 + 80 = 88) does not, and the hatch is asked for the tallest. 96 (3 m) clears every one
    // -- G17: the tallest is now the walker, 2.71 m, its head at 8 + 86.72 = 94.72, still under.
    text += volume_text(6, 6, 96, 120, {"####", "####"});
    const app::MapDoc map = parsed(text);
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    const std::map<Tile, DrawnColumn> drawn
        = drawn_volumes(game::build_scene(sim, looking_at(map.seed, 12, 12, 20)));
    const auto it = drawn.find({7, 7});
    ASSERT_NE(it, drawn.end());
    const float datum = datum_metres(map.seed, 7, 7);
    EXPECT_NEAR(it->second.bottom - datum, 96.0F * game::kMetresPerHeightUnit, 1e-3F)
        << "the underside is 3 m up";
    EXPECT_NEAR(it->second.top - datum, 120.0F * game::kMetresPerHeightUnit, 1e-3F);
    EXPECT_FALSE(
        sim::volume_stops(sim.volumes().at(0).volume, sim::Fixed::zero(), sim::tallest_body()))
        << "every vehicle fits under 96 units (the tallest's head is at 94.72, G16 88)";
    EXPECT_FALSE(game::ground_blocked(sim.volumes(), 7, 7));
    EXPECT_TRUE(game::ground_overhead(sim.volumes(), 7, 7));
}

TEST(MapShapes, TheMapToolShadesASlabYouDriveUnderAndHatchesOneYouCannot) {
    // `protekt-map draw` follows the game's ground rule: hatched where a grounded unit is refused,
    // shaded with a thin red line where a volume is over (or under) a unit that passes.
    std::string text = app::write_map(app::generate_map(3, 24));
    text += volume_text(6, 6, 96, 120, {"#"}); // every vehicle fits under (G16: was 80)
    text += volume_text(10, 6, 30, 60, {"#"}); // too low to
    const app::MapDoc map = parsed(text);
    const std::filesystem::path png = std::filesystem::temp_directory_path()
                                      / ("protekt-shapes-" + std::to_string(::getpid()) + ".png");
    ASSERT_TRUE(app::draw_map(map, png));
    std::vector<std::uint8_t> px;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    ASSERT_TRUE(game::read_png_rgba(png, px, w, h));
    std::filesystem::remove(png);
    const auto at = [&](int tx, int ty, int a, int b) {
        const std::size_t i = ((static_cast<std::size_t>((ty * app::kMapDrawScale) + b) * w)
                               + static_cast<std::size_t>((tx * app::kMapDrawScale) + a))
                              * 4U;
        return std::array<std::uint8_t, 3>{px.at(i), px.at(i + 1), px.at(i + 2)};
    };
    const auto hatch = [](const std::array<std::uint8_t, 3>& c) {
        return c == game::kBlockedHatchBright || c == game::kBlockedHatchDark;
    };
    // The wall: every interior pixel is the hatch.
    for (int b = 2; b < app::kMapDrawScale - 2; ++b) {
        for (int a = 2; a < app::kMapDrawScale - 2; ++a) {
            EXPECT_TRUE(hatch(at(10, 6, a, b))) << "the low slab, pixel " << a << "," << b;
        }
    }
    // The slab overhead: ground, not hatch, between its thin lines; a line on the diagonal.
    EXPECT_FALSE(hatch(at(6, 6, 3, 2)));
    EXPECT_FALSE(hatch(at(6, 6, 2, 4)));
    EXPECT_EQ(at(6, 6, 3, 5), game::kBlockedHatchBright) << "the thin line (3 + 5 = 8)";
}

// ---- (4) THE OLD MAPS LOAD EXACTLY AS BEFORE ------------------------------------------------

TEST(MapShapes, TheProvingGroundsVolumesAreExactlyAsBefore) {
    // The kerb, as it loaded before G15: ONE volume at (30, 24), unturned, 8 x 1, its first six
    // tiles solid, from 0 to 40 units. A `#`/`.` mask carries no digit levels.
    const app::MapDoc map = proving_ground();
    ASSERT_EQ(map.volumes.size(), 1U);
    const app::MapVolume& doc = map.volumes.at(0);
    EXPECT_EQ(doc.x, 30);
    EXPECT_EQ(doc.y, 24);
    EXPECT_FALSE(doc.turned);
    EXPECT_EQ(doc.base, 0);
    EXPECT_EQ(doc.top, 40);
    EXPECT_EQ(doc.mask_width, 8);
    EXPECT_EQ(doc.mask_height, 1);
    EXPECT_EQ(doc.mask_bits, 0b111111U);
    EXPECT_TRUE(doc.levels.empty()) << "a # and . mask is the old form, not a digit one";
    const std::vector<sim::PlacedVolume> placed = app::placed_volumes(map);
    ASSERT_EQ(placed.size(), 1U);
    EXPECT_EQ(placed.at(0).x, 30);
    EXPECT_EQ(placed.at(0).y, 24);
    EXPECT_FALSE(placed.at(0).turned);
    EXPECT_EQ(placed.at(0).volume.extent.width, 8);
    EXPECT_EQ(placed.at(0).volume.extent.height, 1);
    EXPECT_EQ(placed.at(0).volume.extent.bits, 0b111111U);
    EXPECT_EQ(placed.at(0).volume.base.raw, 0);
    EXPECT_EQ(placed.at(0).volume.top.raw, 40 * kOne);
    // And the file writes back byte for byte where its volume is concerned.
    const std::string written = app::write_map(map);
    EXPECT_NE(written.find("base = 0\ntop = 40\nmask = \"\"\"\n######..\n\"\"\"\n"),
              std::string::npos);
}

} // namespace
} // namespace protekt
