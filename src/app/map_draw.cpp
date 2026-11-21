#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "protekt/app/map_file.hpp"
#include "protekt/game/png.hpp"
#include "protekt/sim/steering.hpp"

namespace protekt::app {

namespace {

using Rgb = std::array<int, 3>;

constexpr int kS = kMapDrawScale;

/// Height as a ground colour: low is a deep olive, high a pale sand, so a hill reads as light.
[[nodiscard]] Rgb ground_colour(int height, int low, int span, world::Terrain terrain) {
    const int t = std::clamp(height - low, 0, span) * 256 / span; // 0..256
    Rgb c{40 + t * 170 / 256, 62 + t * 150 / 256, 44 + t * 100 / 256};
    if (terrain == world::Terrain::Rough) {
        for (int& v : c) {
            v = v * 88 / 100;
        }
    } else if (terrain == world::Terrain::Ridge) {
        c[0] = std::min(255, c[0] + 22);
        c[1] = std::min(255, c[1] + 14);
    }
    return c;
}

} // namespace

bool draw_map(const MapDoc& map, const std::filesystem::path& path) {
    const int w = map.width * kS;
    const int h = map.height * kS;
    if (map.width < 1 || map.height < 1) {
        return false;
    }
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U,
                                 255);
    const auto put = [&](int x, int y, const Rgb& c) {
        if (x < 0 || y < 0 || x >= w || y >= h) {
            return;
        }
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
                                + static_cast<std::size_t>(x))
                               * 4U;
        px[at] = static_cast<std::uint8_t>(c[0]);
        px[at + 1] = static_cast<std::uint8_t>(c[1]);
        px[at + 2] = static_cast<std::uint8_t>(c[2]);
        px[at + 3] = 255;
    };
    // Shade over the map's OWN range (at least a tile's worth), so a map that lives between -50 and
    // 120 is not drawn as one flat grey-green.
    const auto [lowest, highest] = std::minmax_element(map.heights.begin(), map.heights.end());
    const int low = *lowest;
    const int span = std::max(*highest - *lowest, sim::kHeightUnitsPerTile);
    const auto height_at = [&](int x, int y) { return map.heights[map.index(x, y)]; };

    // Which tiles a volume covers.
    std::vector<std::uint8_t> covered(map.solid.size(), 0);
    const auto volumes = placed_volumes(map);
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            for (const sim::PlacedVolume& v : volumes) {
                if (sim::placed_solid_at(v, x, y)) {
                    covered[map.index(x, y)] = 1;
                }
            }
        }
    }

    for (int ty = 0; ty < map.height; ++ty) {
        for (int tx = 0; tx < map.width; ++tx) {
            const std::size_t i = map.index(tx, ty);
            const bool crater = map.terrain[i] == world::Terrain::Crater;
            const bool wall = map.solid[i] != 0 || covered[i] != 0;
            for (int py = 0; py < kS; ++py) {
                for (int pxl = 0; pxl < kS; ++pxl) {
                    Rgb c = ground_colour(map.heights[i], low, span, map.terrain[i]);
                    if (wall) {
                        c = ((pxl + py) % 4 < 2) ? Rgb{170, 34, 34} : Rgb{64, 14, 14};
                    } else if (crater) {
                        c = {18, 22, 52};
                    }
                    put(tx * kS + pxl, ty * kS + py, c);
                }
            }
        }
    }

    // Named places, boxed in yellow.
    for (const MapPlace& place : map.places) {
        const int x0 = place.x * kS;
        const int y0 = place.y * kS;
        const int x1 = (place.x + place.w) * kS - 1;
        const int y1 = (place.y + place.h) * kS - 1;
        for (int x = x0; x <= x1; ++x) {
            put(x, y0, {250, 220, 40});
            put(x, y1, {250, 220, 40});
        }
        for (int y = y0; y <= y1; ++y) {
            put(x0, y, {250, 220, 40});
            put(x1, y, {250, 220, 40});
        }
    }

    // Cliffs: an edge too steep to climb is a wall from below, so it is drawn black, on the LOW
    // side, two pixels thick -- the line a person must not expect to drive up.
    for (int ty = 0; ty < map.height; ++ty) {
        for (int tx = 0; tx < map.width; ++tx) {
            const int here = height_at(tx, ty);
            constexpr std::array<std::array<int, 2>, 4> kSides{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
            for (const auto& side : kSides) {
                const int nx = tx + side[0];
                const int ny = ty + side[1];
                if (nx < 0 || ny < 0 || nx >= map.width || ny >= map.height
                    || height_at(nx, ny) - here <= sim::kClimbLimit) {
                    continue; // only the tile that LOOKS UP at a cliff is outlined
                }
                for (int k = 0; k < kS; ++k) {
                    for (int d = 0; d < 2; ++d) {
                        const int along = k;
                        const int across = side[0] != 0 ? (side[0] > 0 ? kS - 1 - d : d)
                                                        : (side[1] > 0 ? kS - 1 - d : d);
                        const int ox = side[0] != 0 ? across : along;
                        const int oy = side[0] != 0 ? along : across;
                        put(tx * kS + ox, ty * kS + oy, {8, 8, 8});
                    }
                }
            }
        }
    }

    // Spawns: a disc with a white ring, green for the player and red for the enemy.
    for (const MapSpawn& spawn : map.spawns) {
        const Rgb fill = spawn.side == "player" ? Rgb{40, 230, 70} : Rgb{240, 50, 50};
        const int cx = spawn.x * kS + kS / 2;
        const int cy = spawn.y * kS + kS / 2;
        for (int dy = -kS / 2; dy <= kS / 2; ++dy) {
            for (int dx = -kS / 2; dx <= kS / 2; ++dx) {
                const int d2 = dx * dx + dy * dy;
                if (d2 <= (kS / 2) * (kS / 2)) {
                    put(cx + dx, cy + dy,
                        d2 >= (kS / 2 - 1) * (kS / 2 - 1) ? Rgb{255, 255, 255} : fill);
                }
            }
        }
    }
    return game::write_png_rgba(path, px, static_cast<std::uint32_t>(w),
                                static_cast<std::uint32_t>(h));
}

} // namespace protekt::app
