#pragma once

#include <cstdint>
#include <vector>

#include "protekt/game/nav_marks.hpp"
#include "protekt/game/scene_data.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

/// THE WORLD MAP (N4): the whole world as a square, on request.
///
/// A PURE VIEW. Opening it changes no state, issues no order and cannot be clicked; it is drawn
/// over the frame and the game underneath carries on exactly as it was. What it shows is the
/// ground (every tile in the colour of its own terrain, taken from the ground's atlas tiles so the
/// two cannot disagree), YOUR assets, and your nav marks with their numbers.
///
/// IT NEVER SHOWS AN ENEMY ASSET. `world_map` takes the side whose map it is and copies only that
/// side's living units into `assets`; there is no other path from a unit to the map, so the one
/// hard rule is a property of one loop, and a test scans the result by id.
///
/// NORTH IS UP and east is right, the way the camera sees the world at yaw 0 (z north, x east), so
/// tile (0, 0) is the map's bottom-left cell and tile (255, 255) its top-right one. The map is a
/// SQUARE with one pixels-per-tile figure for both axes, whatever the screen's aspect.
namespace protekt::game {

inline constexpr int kWorldMapTiles = sim::kMapTiles;

/// The map's square in the frame, in pixels from the left and from the TOP. `pixels_per_tile` is
/// the one scale on both axes; `size == pixels_per_tile * kWorldMapTiles`.
struct WorldMapLayout {
    float x{};
    float y{};
    float size{};
    float pixels_per_tile{};
};

/// The fraction of the room the square takes (room: the width, and the height under the roster
/// strip and above the caption), centred.
inline constexpr float kWorldMapFraction = 0.96F;

/// Marks and dots are a FIXED size in pixels of a 720-high frame (they scale with the frame height
/// like the roster strip), so they read the same at any map scale.
inline constexpr float kMapUnitHalf = 3.0F;
inline constexpr float kMapBuildingHalf = 5.0F;
inline constexpr float kMapDrivenRingHalf = 8.0F;
inline constexpr float kMapMarkHalf = 10.0F; ///< a mark's plate: 20 px square
inline constexpr float kMapMarkCell = 3.0F;  ///< a digit is 5x5 cells, so 15 px tall

[[nodiscard]] WorldMapLayout world_map_layout(Extent extent) noexcept;

/// A point in the frame, in pixels from the left and from the top.
struct MapPoint {
    float x{};
    float y{};
};

/// A tile of the world: x east, y north, each in [0, kWorldMapTiles).
struct MapCell {
    int col{};
    int row{};
    friend bool operator==(const MapCell&, const MapCell&) = default;
};

/// Where a world position (in tiles, fractional) falls in the frame.
[[nodiscard]] MapPoint map_point(const WorldMapLayout& layout, float tile_x, float tile_y) noexcept;

/// The tile under a point of the map, clamped onto the map.
[[nodiscard]] MapCell map_cell(const WorldMapLayout& layout, MapPoint point) noexcept;

struct MapColour {
    float r{};
    float g{};
    float b{};
    friend bool operator==(const MapColour&, const MapColour&) = default;
};

/// The colour a terrain type has on the map: the mean of its family's ground tiles in the atlas.
[[nodiscard]] MapColour map_terrain_colour(world::Terrain terrain);

/// The terrain of every tile of the world for `seed`, row-major from the south-west corner
/// (`[y * kWorldMapTiles + x]`). The world is a pure function of its seed, so the grid for the last
/// seed asked about is kept and handed back: a frame does not regenerate sixteen chunks to draw it.
[[nodiscard]] const std::vector<world::Terrain>& map_terrain_grid(std::uint64_t seed);

/// One of your own living units or buildings, centred at (px, py), `half` the dot's half-size
/// (in frame pixels at the current frame height). `driven` is the one you are sitting in.
struct MapAsset {
    sim::UnitId id{};
    sim::UnitKind kind{};
    float px{};
    float py{};
    float half{};
    bool driven{false};
};

/// A nav mark: the centre of its tile, a plate `half` out from it, its digit in `cell`-pixel cells.
struct MapMark {
    int number{};
    float px{};
    float py{};
    float half{};
    float cell{};
};

struct WorldMap {
    WorldMapLayout layout{};
    std::vector<MapAsset> assets;
    std::vector<MapMark> marks;
};

/// Everything the map draws apart from the ground, for the side `own` and the unit `driven`.
/// `marks` may be null. Only living units of `own` are included; marks only if dropped.
[[nodiscard]] WorldMap world_map(const sim::Sim& sim, Extent extent, sim::Side own,
                                 sim::UnitId driven, const NavMarks* marks);

} // namespace protekt::game
