#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "protekt/game/nav_marks.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/scene_data.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

/// THE WORLD MAP (N4): the whole world as a square, on request.
///
/// A PURE VIEW. Opening it changes no state, issues no order and cannot be clicked; it is drawn
/// over the frame and the game underneath carries on exactly as it was. What it shows is the
/// ground (every tile in the colour of its own terrain, taken from the ground's atlas tiles so the
/// two cannot disagree), YOUR assets, and your nav marks, drawn as the radar draws them.
///
/// IT NEVER SHOWS AN ENEMY ASSET. `world_map` takes the side whose map it is and copies only that
/// side's living units into `assets`; there is no other path from a unit to the map, so the one
/// hard rule is a property of one loop, and a test scans the result by id.
///
/// WHAT IT SHOWS OF THE ENEMY IS THE RADAR'S BLIPS and nothing else (Harri, 2026-11-03): `blips`,
/// from `radar_detections` -- the radar's own model, so the two views cannot disagree. A moving
/// enemy in the driven unit's reach is there always; a stationary one only as a sweep passes, and
/// it fades within a second. A blip is a sighting, not an asset: it is in its own list.
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

/// THE SQUARE IS HALF THE RADAR'S MAJOR DIAMETER. The map and the radar swap into the same
/// bottom-left corner; the radar's long axis, `radar_panel(extent).width` (the
/// `kRadarWidthFraction` of the frame's width, clamped), is the measure, and the map is
/// `kWorldMapRadarFraction` of it on each side. It is a SQUARE, so its height is that too; the
/// frame HEIGHT only limits it on a frame too short to hold the square. History of the owner's
/// calls: 2026-10-23 "The map size should match the major diameter of the elliptical radar display"
/// (the earlier 12.7% of the frame width read "a tiny bit smaller than what I expected"), then
/// 2026-11-03 "The map is too big. It should be about half the height and width": 330.24 px at
/// 1280x720 became 165.12.
inline constexpr float kWorldMapRadarFraction = 0.5F;

/// The panel size the furniture constants below were drawn for: BZ2's 12.7% minimap square at 1280
/// wide. It stays the reference now the square is the radar's diameter, so the furniture GROWS with
/// the panel instead of shrinking relative to it -- 162.56 px is what "1.0" means, whatever the
/// square's current side.
inline constexpr float kWorldMapReferencePixels = 162.56F;

/// Furniture scales with the PANEL, not the frame: its size is the constant below times
/// `map_furniture_scale(layout)`. The floor keeps a unit's dot visible in a small window's panel,
/// the ceiling keeps a 4K panel's blobs blobs rather than slabs.
inline constexpr float kMapMinFurnitureScale = 0.6F;
inline constexpr float kMapMaxFurnitureScale = 1.6F;

/// Marks and dots at the REFERENCE panel size; multiply by `map_furniture_scale(layout)`. So they
/// keep their share of the map when the panel grows or shrinks, instead of staying fixed in frame
/// pixels and overwhelming a small minimap. A vehicle's dot is 1.5 a side-half, half of the 3.0 it
/// was (Harri, round 10, 2026-11-05: "half the width and height of the current size", on the map as
/// on the radar). The MAP's dots then went a third of the way back (round 11, AE: "slightly larger
/// ... a bit too small now"): 3.0 -> 1.5 -> 2.0 for a vehicle, 10.0 -> 5.0 -> 6.5 for a building.
/// The RADAR's sizes are its own fractions of its panel (`radar.cpp`) and did not move. The heading
/// needle is scaled by the panel's furniture scale (`asset.half` over this), so resizing the dot
/// leaves the needle its 3 mm.
inline constexpr float kMapUnitHalf = 2.0F;
inline constexpr float kMapBuildingHalf = 6.5F;
/// A BIOMETAL POOL's marker (Harri, 2026-11-02: "the map shows the locations of all biometal
/// pools"): a hollow square, `kMapPoolHalf` out from the pool's middle and `kMapPoolRing` thick,
/// white inside a black edge. As big as a building's square so it stands clear of the blip of the
/// scavenger deployed on it, and hollow so it reads as a place and not as one of your units.
inline constexpr float kMapPoolHalf = kMapBuildingHalf;
inline constexpr float kMapPoolRing = 1.5F;

/// The driven unit's direction line: 3 mm on the reference frame. A frame pixel is treated as a CSS
/// pixel at 96 dpi (1280 px across is 13.3 in), so 3 mm is 3 / 25.4 * 96 = 11.34 px. The line runs
/// from the blip's edge outward along the heading and is scaled by the SAME factor as the blip it
/// belongs to -- never by the panel's furniture scale, which would stretch it to 1.6x and stop it
/// being 3 mm. The owner asked for "a 3mm line pointing straight ahead" (2026-10-23).
inline constexpr float kMapHeadingLinePixels = 11.34F;
/// Half the direction line's width, in the same reference pixels: 1.8 px wide, a needle not a
/// wedge.
inline constexpr float kMapHeadingLineHalfPixels = 0.9F;

/// The square's geometry: bottom-left corner at the panel margin, `size` = the width fraction.
[[nodiscard]] WorldMapLayout world_map_layout(Extent extent) noexcept;

/// How much to multiply the furniture constants by for this panel: the panel's size against the
/// reference, clamped to the readable band above.
[[nodiscard]] float map_furniture_scale(const WorldMapLayout& layout) noexcept;

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

/// A nav mark: the centre of its tile and the plus's half-size, which is the radar's own
/// (`radar_nav_half`) -- his ruling, 2026-10-26: the mark is "EXACTLY the same as in the radar".
struct MapMark {
    int slot{};
    float px{};
    float py{};
    float half{};
    bool highlighted{false};
};

/// WHICH WAY THE MAP'S WHITE LINE POINTS, as one named choice (Harri, 2026-11-01). He ruled that
/// the line shows where he WOULD MOVE going forward -- the HULL's heading -- and not where the
/// turret is aimed ("that sounds wrong to me, but I think it's the right choice"), and he asked to
/// be able to flip it to the MUZZLE's direction later. THIS CONSTANT IS THAT FLIP: change `Hull` to
/// `Muzzle` and nothing else.
enum class MapLineAims : std::uint8_t { Hull, Muzzle };
inline constexpr MapLineAims kMapLineAims = MapLineAims::Hull;

/// The heading, in the `Headings` convention (0 north, positive toward east), the driven unit's
/// white line on the map points along, for `aims`. The unit's FACING (`SceneOptions::facing`: the
/// hull's heading and the turret's angle from it) answers while it is turning on the spot, which a
/// velocity cannot: the hull's own, or the hull's plus the turret's for the muzzle. A unit with no
/// facing -- a headless or observer frame -- falls back on the velocity heading the headings table
/// remembers, as the line always read it. `aims` defaults to `kMapLineAims`; passing the other
/// choice is for the test that pins the alternative.
[[nodiscard]] float map_line_heading(const SceneOptions& options, const sim::Unit& unit,
                                     MapLineAims aims = kMapLineAims);

/// The driven unit's direction needle: the four corners of a thin white quad running from the
/// blip's edge outward along its heading, in the frame's pixels from the left and from the TOP.
///
/// DIRECTION CONVENTION. A heading is the sim's own yaw in radians: 0 is the world's forward, and
/// `player.cpp` drives a tank forward as `(sin h, cos h)` in world x/y. The map puts north UP and
/// flips the world's y into the frame's y (`map_point`), so the same motion in screen space is
/// `(sin h, -cos h)`: a tank facing north draws its needle UP the map (negative py), one facing
/// east draws it right. `half` is the blip's half-size as `MapAsset::half` carries it, so the
/// needle keeps its proportion to the blip at any frame size and stays 3 mm on the reference frame.
///
/// The needle starts at the blip's edge and is `kMapHeadingLinePixels` long relative to
/// `kMapUnitHalf`, and its two sides are `kMapHeadingLineHalfPixels` out from the centre line. It
/// does not include the blip itself: the blip is the square the asset draws, the needle points out
/// of its edge. A blip bigger than its own needle still reads: the needle's length grows with it.
[[nodiscard]] std::array<MapPoint, 4> map_heading_needle(const MapAsset& asset,
                                                         float heading_radians) noexcept;

/// A pool's marker, centred at (px, py), `half` its half-size in frame pixels at the current frame
/// height. ALL the world's pools, in the sim's order: a pool is terrain, not an asset, so unlike an
/// enemy unit there is nothing to hide, and the map shows the ones out of sight too.
struct MapPool {
    float px{};
    float py{};
    float half{};
};

/// An enemy the radar is showing right now, centred at (px, py), `half` its half-size in frame
/// pixels, `alpha` how strongly (`RadarDetection::alpha`).
struct MapBlip {
    sim::UnitId id{};
    float px{};
    float py{};
    float half{};
    float alpha{};
};

struct WorldMap {
    WorldMapLayout layout{};
    std::vector<MapAsset> assets;
    std::vector<MapBlip> blips;
    std::vector<MapMark> marks;
    std::vector<MapPool> pools;
};

/// Everything the map draws apart from the ground, for the side `own` and the unit `driven`.
/// `marks` may be null. Only living units of `own` are included as assets; the enemy appears only
/// as the radar's `blips` (empty when `driven` is not a unit of the sim); marks only if placed;
/// every pool. `activity` (may be null) is what the radar knows of who moved or fired lately; the
/// blips are the radar's detections made with it.
[[nodiscard]] WorldMap world_map(const sim::Sim& sim, Extent extent, sim::Side own,
                                 sim::UnitId driven, const NavMarks* marks,
                                 const RadarActivity* activity = nullptr);

} // namespace protekt::game
