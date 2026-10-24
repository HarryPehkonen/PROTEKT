#include "protekt/game/world_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "protekt/game/atlas.hpp"
#include "protekt/game/radar.hpp" // radar_panel: the map takes the oval's own major diameter
#include "protekt/game/scene.hpp"

namespace protekt::game {

namespace {

/// A point exactly on a cell's edge (a unit standing on a whole tile coordinate) belongs to the
/// cell that starts there; float rounding must not tip it into the one before.
constexpr float kCellSlack = 1.0e-3F;
constexpr float kReferenceHeight = 720.0F;

[[nodiscard]] float tiles(const sim::Fixed& value) noexcept {
    return static_cast<float>(value.raw) / static_cast<float>(sim::Fixed::kOne);
}

/// The mean colour of each terrain family's ground tiles, once: the same atlas the ground samples.
[[nodiscard]] const std::array<MapColour, world::kTerrainCount>& palette() {
    static const std::array<MapColour, world::kTerrainCount> kPalette = [] {
        std::array<MapColour, world::kTerrainCount> colours{};
        const std::vector<std::uint8_t> pixels = atlas_pixels();
        const AtlasManifest& manifest = atlas_manifest();
        for (int type = 0; type < world::kTerrainCount; ++type) {
            double red = 0.0;
            double green = 0.0;
            double blue = 0.0;
            double count = 0.0;
            for (const std::string_view name :
                 kGroundVariantNames.at(static_cast<std::size_t>(type))) {
                const TileCell cell = tile_cell(manifest.tile(name));
                for (std::uint32_t y = 0; y < kTileSide; ++y) {
                    for (std::uint32_t x = 0; x < kTileSide; ++x) {
                        const std::size_t at
                            = (static_cast<std::size_t>((cell.row * kTileSide) + y) * kAtlasSide
                               + ((cell.col * kTileSide) + x))
                              * 4U;
                        red += pixels.at(at);
                        green += pixels.at(at + 1U);
                        blue += pixels.at(at + 2U);
                        count += 1.0;
                    }
                }
            }
            colours.at(static_cast<std::size_t>(type)) = MapColour{
                static_cast<float>(red / count / 255.0), static_cast<float>(green / count / 255.0),
                static_cast<float>(blue / count / 255.0)};
        }
        return colours;
    }();
    return kPalette;
}

} // namespace

WorldMapLayout world_map_layout(Extent extent) noexcept {
    const float height = static_cast<float>(extent.height);
    // The corner inset is a fixed 24 px of the 720-high frame the UI was drawn at, so it scales
    // with the frame height like the roster strip. The square's side is the RADAR'S MAJOR DIAMETER:
    // the two views swap into the same corner, so the map takes the elliptical display's long axis
    // rather than a fraction of its own (the owner's call, see world_map.hpp). The frame HEIGHT
    // only caps it on a frame too short to hold the square.
    const float margin = kPanelMarginPixels * (height / kReferenceHeight);
    const float side = std::min(radar_panel(extent).width, height - (2.0F * margin));
    const float per_tile = std::max(side / static_cast<float>(kWorldMapTiles),
                                    1.0F / static_cast<float>(kWorldMapTiles));
    const float size = per_tile * static_cast<float>(kWorldMapTiles);
    // Bottom-left, where BZ2 puts its minimap and where the radar also sits: the two views swap
    // into the same corner rather than each claiming its own.
    return WorldMapLayout{margin, height - margin - size, size, per_tile};
}

float map_furniture_scale(const WorldMapLayout& layout) noexcept {
    const float ratio = layout.size / kWorldMapReferencePixels;
    return std::clamp(ratio, kMapMinFurnitureScale, kMapMaxFurnitureScale);
}

MapPoint map_point(const WorldMapLayout& layout, float tile_x, float tile_y) noexcept {
    return MapPoint{layout.x + (tile_x * layout.pixels_per_tile),
                    layout.y + layout.size - (tile_y * layout.pixels_per_tile)};
}

MapCell map_cell(const WorldMapLayout& layout, MapPoint point) noexcept {
    const auto cell = [&](float offset) {
        return std::clamp(
            static_cast<int>(std::floor((offset / layout.pixels_per_tile) + kCellSlack)), 0,
            kWorldMapTiles - 1);
    };
    return MapCell{cell(point.x - layout.x), cell(layout.y + layout.size - point.y)};
}

const std::vector<world::Terrain>& map_terrain_grid(std::uint64_t seed) {
    static thread_local std::uint64_t cached_seed{};
    static thread_local std::vector<world::Terrain> cached;
    if (!cached.empty() && cached_seed == seed) {
        return cached;
    }
    constexpr int kChunks = kWorldMapTiles / world::kChunkSize;
    constexpr std::size_t kSide = static_cast<std::size_t>(kWorldMapTiles);
    constexpr std::size_t kChunkSide = static_cast<std::size_t>(world::kChunkSize);
    cached.assign(static_cast<std::size_t>(kWorldMapTiles) * kWorldMapTiles, world::Terrain::Flat);
    for (int cy = 0; cy < kChunks; ++cy) {
        for (int cx = 0; cx < kChunks; ++cx) {
            const world::Chunk chunk = world::generate(seed, cx, cy);
            for (int ty = 0; ty < world::kChunkSize; ++ty) {
                for (int tx = 0; tx < world::kChunkSize; ++tx) {
                    const int x = (cx * world::kChunkSize) + tx;
                    const int y = (cy * world::kChunkSize) + ty;
                    cached.at((static_cast<std::size_t>(y) * kSide) + static_cast<std::size_t>(x))
                        = chunk.tiles
                              .at((static_cast<std::size_t>(ty) * kChunkSide)
                                  + static_cast<std::size_t>(tx))
                              .terrain;
                }
            }
        }
    }
    cached_seed = seed;
    return cached;
}

MapColour map_terrain_colour(world::Terrain terrain) {
    const auto at = static_cast<std::size_t>(terrain);
    return palette().at(at < palette().size() ? at : 0U);
}

WorldMap world_map(const sim::Sim& sim, Extent extent, sim::Side own, sim::UnitId driven,
                   const NavMarks* marks) {
    WorldMap map;
    map.layout = world_map_layout(extent);
    const float scale = static_cast<float>(extent.height) / kReferenceHeight;
    // THE ONE LOOP from a unit to the map, and it keeps only the viewer's own living units.
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side != own) {
            continue;
        }
        const MapPoint at = map_point(map.layout, tiles(unit.x), tiles(unit.y));
        const bool building
            = unit.kind == sim::UnitKind::Factory || unit.kind == sim::UnitKind::Turret;
        map.assets.push_back(MapAsset{unit.id, unit.kind, at.x, at.y,
                                      (building ? kMapBuildingHalf : kMapUnitHalf) * scale,
                                      unit.id == driven});
    }
    if (marks != nullptr) {
        for (const NavMarks::Mark& mark : marks->marks()) {
            const MapPoint at = map_point(map.layout, static_cast<float>(mark.tile_x) + 0.5F,
                                          static_cast<float>(mark.tile_y) + 0.5F);
            map.marks.push_back(
                MapMark{mark.number, at.x, at.y, kMapMarkHalf * scale, kMapMarkCell * scale});
        }
    }
    return map;
}

std::array<MapPoint, 4> map_heading_needle(const MapAsset& asset, float heading_radians) noexcept {
    // The sim's heading drives a tank forward as world (sin h, cos h); the map's y is flipped into
    // the frame's y, so the needle points along (sin h, -cos h) in screen space. See world_map.hpp.
    const float dir_x = std::sin(heading_radians);
    const float dir_y = -std::cos(heading_radians);
    // The needle's length and width grow with the blip it belongs to, so the 3 mm reference length
    // stays 3 mm only at the reference blip size and the needle always reads as the blip's own.
    const float blip_scale = asset.half / kMapUnitHalf;
    const float from = asset.half;
    const float to = asset.half + (kMapHeadingLinePixels * blip_scale);
    const float wing = kMapHeadingLineHalfPixels * blip_scale;
    const auto at = [&](float along, float across) {
        return MapPoint{asset.px + (dir_x * along) - (dir_y * across),
                        asset.py + (dir_y * along) + (dir_x * across)};
    };
    // The four corners of the thin quad: base-left, base-right, tip-right, tip-left. Order matches
    // emit_quad's winding so the face points at the camera on the map's HUD layer.
    return std::array<MapPoint, 4>{at(from, wing), at(from, -wing), at(to, -wing), at(to, wing)};
}

} // namespace protekt::game
