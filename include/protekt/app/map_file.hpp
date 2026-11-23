#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/climate.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/ground.hpp"
#include "protekt/world/world.hpp"

namespace protekt::app {

/// A MAP IS A DOCUMENT (M6). The file is a small strict subset of TOML: a header that names the map
/// and where it came from, and RASTERS that a person can read and edit in a text editor.
///
///   [map]                 name = "...", seed = N, width = W, height = H   (W, H in 2..256)
///                         climate = "temperate" | "sand" | "ice" | "regolith"   (optional)
///   [terrain]  rows = """  H rows of W letters: f flat, r rough, R ridge, c crater
///   [height]   rows = """  H rows of W integers (-512..512), each right-aligned in a 4-wide column
///   [solid]    rows = """  H rows of W letters: # impassable tile, . open  (the WORLD frame)
///   [[spawn]]   side = "player" | "enemy", x, y
///   [[place]]   name = "...", x, y, w, h        (a named rectangle: "the ambush", "the ramp")
///   [[volume]]  x, y, turned, base, top, mask = """ rows of # and . """    (one solid thing: an
///               extent in its OWN frame set down on the world at (x, y), and the heights it
///               occupies; the very `sim::PlacedVolume`, and a building's mask is the same letters)
///
/// Rasters are plain text so a diff of two maps shows exactly which tiles moved. `seed` records
/// where a generated map came from, and survives hand edits, which never touch it.
struct MapSpawn {
    std::string side{}; ///< "player" or "enemy"
    int x{};
    int y{};
    bool operator==(const MapSpawn&) const = default;
};

struct MapPlace {
    std::string name{};
    int x{};
    int y{};
    int w{};
    int h{};
    bool operator==(const MapPlace&) const = default;
};

struct MapVolume {
    int x{};
    int y{};
    bool turned{};
    int base{}; ///< height units above the local ground
    int top{};
    int mask_width{};
    int mask_height{};
    std::uint64_t mask_bits{}; ///< row-major, bit `ly * width + lx`, as `sim::SolidMask`
    bool operator==(const MapVolume&) const = default;
};

struct MapDoc {
    std::string name{};
    std::uint64_t seed{};
    int width{};
    int height{};
    /// The map's climate: a look (ground, sky, fog), never a rule. Absent from the header means
    /// Temperate, the look every map had before climates existed; a value that is not a climate is
    /// refused by name.
    game::Climate climate{game::Climate::Temperate};
    std::vector<world::Terrain> terrain{}; ///< width * height, row-major
    std::vector<int> heights{};            ///< width * height, row-major
    std::vector<std::uint8_t> solid{};     ///< width * height, 1 = impassable
    std::vector<MapSpawn> spawns{};
    std::vector<MapPlace> places{};
    std::vector<MapVolume> volumes{};
    bool operator==(const MapDoc&) const = default;

    [[nodiscard]] std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
               + static_cast<std::size_t>(x);
    }
};

inline constexpr int kMapMinSide = 2;
inline constexpr int kMapMaxSide = 256;

/// The map a seed makes: chunk (0, 0) of the seed's world cropped to `size` x `size` (clamped to
/// 2..64), with the first and last spawnable tiles as the two spawns. A pure function of its
/// arguments; the seed is recorded in the document.
[[nodiscard]] MapDoc generate_map(std::uint64_t seed, int size);

/// The document as text. A pure function of the document: two writes are byte-identical.
[[nodiscard]] std::string write_map(const MapDoc& map);

struct MapParse {
    MapDoc map{};
    std::string error{}; ///< "line N: ..." when not ok
    bool ok{false};
};

/// Reads a map from text. Strict: anything it does not understand is refused with its line.
[[nodiscard]] MapParse parse_map(std::string_view text);
[[nodiscard]] MapParse load_map_file(const std::filesystem::path& path);

/// The map's volumes as the sim's own world-anchored volumes (`Scenario::volumes`).
[[nodiscard]] std::vector<sim::PlacedVolume> placed_volumes(const MapDoc& map);

/// THE MAP AS THE GROUND A BATTLE IS PLAYED ON: its terrain, heights and solid mask, as the
/// `world::MapGround` the sim and the renderer read (`world::ScopedGround`). Tile (x, y) of the
/// file is world tile (x, y); outside the file the world is impassable.
[[nodiscard]] world::MapGround map_ground(const MapDoc& map);

/// `base` with the map's own wiring set on it: the first "player" and the first "enemy" `[[spawn]]`
/// become the two bases (a side with none keeps the seed's draw), and the volumes are handed to the
/// sim. Later spawns and the named places are read by the parser and used by nothing here.
[[nodiscard]] sim::ScenarioData map_scenario_data(const MapDoc& map, sim::ScenarioData base);

/// Pixels per tile in a `draw_map` image.
inline constexpr int kMapDrawScale = 8;

/// Renders the map to a PNG of (width * kMapDrawScale) x (height * kMapDrawScale): ground shaded
/// by height, impassable ground (crater terrain, `[solid]`, a volume's footprint) hatched dark red,
/// craters blue-black, cliff edges (a rise too steep to climb) outlined, named places boxed, spawns
/// as a green (player) or red (enemy) disc. The bytes are a pure function of the map.
[[nodiscard]] bool draw_map(const MapDoc& map, const std::filesystem::path& path);

} // namespace protekt::app
