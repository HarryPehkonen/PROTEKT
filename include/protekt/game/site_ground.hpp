#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "protekt/sim/units.hpp"

namespace protekt::game {

/// A DEPLOYED THING ALTERS THE GROUND AROUND IT (Harri, round 1 on the recycler: "change the tiles
/// under it to look different. Kind of like those tiles became part of the installation. I can
/// still drive over them, but not crash into the recycler"; round 3 on the scavenger: "it should
/// change the atlas of the tiles around it ... it should look like the ground was altered").
///
/// ONE mechanism, three consumers. A site is a thing that stands on the map and marks the world
/// tiles round it: the deployed recycler, the deployed scavenger, and the biometal pool. Each KIND
/// says how far the alteration reaches and which atlas tile it paints (`site_ground`), and
/// `SiteGroundMap` answers, per world tile, which kind (if any) owns it. The scene paints those
/// tiles in place of the ordinary ground. It is PRESENTATION ONLY: the world's terrain, the sim's
/// passability and its hash never see it, so every altered tile stays drivable.
enum class SiteKind : std::uint8_t { Pool = 0, DeployedScavenger = 1, DeployedRecycler = 2 };

/// How one kind alters the ground. `core` is the side, in tiles, of the square the thing itself
/// stands on (a building's is the sim's footprint, a pool and a scavenger stand on one tile);
/// `reach` is how many tiles the alteration extends past that square on every side; `tile` is the
/// atlas manifest name painted there. The altered square is `core + 2 * reach` tiles across.
struct SiteGround {
    int core{};
    int reach{};
    std::string_view tile;
};

/// THE TABLE: the one place a kind's reach and look are written. The kinds nest on purpose. A pool
/// alters a 5x5 patch (a vent: dark sump ground); the scavenger deployed on it adds a 3x3 rig of
/// plating (it wins the tiles they share, so the vent shows round it); the deployed recycler's pad
/// is its 2x2 footprint and one tile of plating beyond, 4x4. A kind's own order in `SiteKind` is
/// its priority where two overlap: the later kind wins the tile.
inline constexpr std::array<SiteGround, 3> kSiteGround{{
    {1, 2, "site_vent"},                          // Pool
    {1, 1, "site_rig"},                           // DeployedScavenger
    {sim::kBuildingFootprintTiles, 1, "site_pad"} // DeployedRecycler
}};

inline constexpr std::size_t kSiteKindCount = kSiteGround.size();

[[nodiscard]] constexpr const SiteGround& site_ground(SiteKind kind) noexcept {
    return kSiteGround.at(static_cast<std::size_t>(kind));
}

/// An inclusive rectangle of world tiles.
struct TileRange {
    int x0{};
    int y0{};
    int x1{};
    int y1{};
    [[nodiscard]] constexpr bool covers(int tile_x, int tile_y) const noexcept {
        return tile_x >= x0 && tile_x <= x1 && tile_y >= y0 && tile_y <= y1;
    }
};

/// The tiles a site of `kind` alters, from the tile it is anchored on: the tile a pool or a
/// scavenger stands on, or the tile a building's position floors to (`footprint_around` takes the
/// footprint's north-west corner `core / 2` tiles back from it, and so does this).
[[nodiscard]] constexpr TileRange site_range(SiteKind kind, int anchor_x, int anchor_y) noexcept {
    const SiteGround& ground = site_ground(kind);
    const int back = (ground.core / 2) + ground.reach;
    const int span = ground.core + (2 * ground.reach) - 1;
    return TileRange{anchor_x - back, anchor_y - back, anchor_x - back + span,
                     anchor_y - back + span};
}

/// Which kind owns each altered world tile: every site `add`ed marks its range, and a tile two
/// sites reach belongs to the stronger kind whatever order they were added in.
class SiteGroundMap {
public:
    void add(SiteKind kind, int anchor_x, int anchor_y);

    /// The kind that owns the tile, or nothing for ordinary ground.
    [[nodiscard]] std::optional<SiteKind> at(int tile_x, int tile_y) const;

    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }

private:
    std::map<std::pair<int, int>, SiteKind> tiles_;
};

} // namespace protekt::game
