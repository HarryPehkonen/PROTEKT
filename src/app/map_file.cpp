#include "protekt/app/map_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include "protekt/app/args.hpp"

namespace protekt::app {

namespace {

using world::Terrain;

constexpr std::string_view kRowsOpen = "\"\"\"";
constexpr int kMaxMaskSide = 8; ///< a `sim::SolidMask` is at most 8 x 8 (64 bits)

[[nodiscard]] char terrain_letter(Terrain terrain) noexcept {
    switch (terrain) {
    case Terrain::Flat:
        return 'f';
    case Terrain::Rough:
        return 'r';
    case Terrain::Ridge:
        return 'R';
    case Terrain::Crater:
        return 'c';
    }
    return 'f';
}

[[nodiscard]] bool terrain_from_letter(char letter, Terrain& terrain) noexcept {
    switch (letter) {
    case 'f':
        terrain = Terrain::Flat;
        return true;
    case 'r':
        terrain = Terrain::Rough;
        return true;
    case 'R':
        terrain = Terrain::Ridge;
        return true;
    case 'c':
        terrain = Terrain::Crater;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::string quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + '"';
}

void append_rows(std::string& out, const std::vector<std::string>& rows) {
    out += "rows = \"\"\"\n";
    for (const std::string& row : rows) {
        out += row;
        out += '\n';
    }
    out += "\"\"\"\n";
}

// ---------------------------------------------------------------------------------------- reading

struct Entry {
    std::string key{};
    std::string text{}; ///< the value as written (a quoted string keeps its quotes)
    std::vector<std::string> rows{};
    int line{};
    bool is_rows{false};
};

struct Section {
    std::string name{};
    bool array{false};
    int line{};
    std::vector<Entry> entries{};
};

struct Failure {
    int line{};
    std::string message{};
};

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Splits the text into sections of `key = value` entries, a `"""` value reading rows verbatim up
/// to the closing `"""`. No meaning is given to anything here.
[[nodiscard]] bool read_sections(std::string_view text, std::vector<Section>& sections,
                                 Failure& failure) {
    std::istringstream in{std::string{text}};
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        const std::string_view row = trim(raw);
        if (row.empty() || row.front() == '#') {
            continue;
        }
        if (row.front() == '[') {
            const bool array = row.starts_with("[[");
            const std::string_view want_end = array ? "]]" : "]";
            if (!row.ends_with(want_end) || row.size() <= 2 * want_end.size()) {
                failure = {line, "a table header is [name] or [[name]]"};
                return false;
            }
            const std::string_view name
                = row.substr(want_end.size(), row.size() - 2 * want_end.size());
            sections.push_back(Section{std::string{name}, array, line, {}});
            continue;
        }
        if (sections.empty()) {
            failure = {line, "a key before any table header"};
            return false;
        }
        const std::size_t eq = row.find('=');
        if (eq == std::string_view::npos) {
            failure = {line, "expected 'key = value'"};
            return false;
        }
        Entry entry;
        entry.key = std::string{trim(row.substr(0, eq))};
        entry.text = std::string{trim(row.substr(eq + 1))};
        entry.line = line;
        if (entry.key.empty()) {
            failure = {line, "a value with no key"};
            return false;
        }
        if (entry.text == kRowsOpen) {
            entry.is_rows = true;
            bool closed = false;
            while (std::getline(in, raw)) {
                ++line;
                if (!raw.empty() && raw.back() == '\r') {
                    raw.pop_back();
                }
                if (trim(raw) == kRowsOpen) {
                    closed = true;
                    break;
                }
                entry.rows.push_back(raw);
            }
            if (!closed) {
                failure = {entry.line, "the \"\"\" that opens these rows is never closed"};
                return false;
            }
        }
        auto& list = sections.back().entries;
        if (std::any_of(list.begin(), list.end(),
                        [&](const Entry& other) { return other.key == entry.key; })) {
            failure = {entry.line, "'" + entry.key + "' is given twice in one table"};
            return false;
        }
        list.push_back(std::move(entry));
    }
    return true;
}

/// A section's entries, claimed one at a time so that what is left over can be refused.
class Fields {
public:
    Fields(const Section& section, Failure& failure) : section_(&section), failure_(&failure) {}

    [[nodiscard]] const Entry* find(const std::string& key) {
        for (const Entry& entry : section_->entries) {
            if (entry.key == key) {
                used_.push_back(key);
                return &entry;
            }
        }
        *failure_ = {section_->line, "[" + section_->name + "] needs '" + key + "'"};
        return nullptr;
    }

    [[nodiscard]] bool integer(const std::string& key, int& value, int low, int high) {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        long long parsed = 0;
        if (entry->is_rows || !parse_number(entry->text, parsed) || parsed < low || parsed > high) {
            *failure_ = {entry->line, "'" + key + "' wants an integer from " + std::to_string(low)
                                          + " to " + std::to_string(high)};
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }

    [[nodiscard]] bool boolean(const std::string& key, bool& value) {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        if (entry->text != "true" && entry->text != "false") {
            *failure_ = {entry->line, "'" + key + "' wants true or false"};
            return false;
        }
        value = entry->text == "true";
        return true;
    }

    [[nodiscard]] bool string(const std::string& key, std::string& value) {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        const std::string& t = entry->text;
        if (entry->is_rows || t.size() < 2 || t.front() != '"' || t.back() != '"') {
            *failure_ = {entry->line, "'" + key + "' wants a double-quoted string"};
            return false;
        }
        value.clear();
        for (std::size_t i = 1; i + 1 < t.size(); ++i) {
            if (t[i] == '\\') {
                ++i;
                if (i + 1 >= t.size() || (t[i] != '"' && t[i] != '\\')) {
                    *failure_ = {entry->line, "'" + key + "': only \\\" and \\\\ are escapes"};
                    return false;
                }
            } else if (t[i] == '"') {
                *failure_
                    = {entry->line, "'" + key + "': a quote inside a string needs a backslash"};
                return false;
            }
            value += t[i];
        }
        return true;
    }

    [[nodiscard]] const std::vector<std::string>* rows() {
        const Entry* entry = find("rows");
        if (entry == nullptr) {
            return nullptr;
        }
        if (!entry->is_rows) {
            *failure_ = {entry->line, "'rows' wants a \"\"\" block"};
            return nullptr;
        }
        return &entry->rows;
    }

    [[nodiscard]] int line_of(const std::string& key) const {
        for (const Entry& entry : section_->entries) {
            if (entry.key == key) {
                return entry.line;
            }
        }
        return section_->line;
    }

    /// Every key the table has must have been asked for.
    [[nodiscard]] bool nothing_left() {
        for (const Entry& entry : section_->entries) {
            if (std::find(used_.begin(), used_.end(), entry.key) == used_.end()) {
                *failure_
                    = {entry.line, "unknown key '" + entry.key + "' in [" + section_->name + "]"};
                return false;
            }
        }
        return true;
    }

private:
    const Section* section_;
    Failure* failure_;
    std::vector<std::string> used_{};
};

[[nodiscard]] bool check_grid(const std::vector<std::string>& rows, int height, int line,
                              Failure& failure) {
    if (static_cast<int>(rows.size()) != height) {
        failure = {line, "expected " + std::to_string(height) + " rows, found "
                             + std::to_string(rows.size())};
        return false;
    }
    return true;
}

[[nodiscard]] bool read_terrain(const std::vector<std::string>& rows, int line, MapDoc& map,
                                Failure& failure) {
    if (!check_grid(rows, map.height, line, failure)) {
        return false;
    }
    for (int y = 0; y < map.height; ++y) {
        const std::string& row = rows[static_cast<std::size_t>(y)];
        if (static_cast<int>(row.size()) != map.width) {
            failure = {line + 1 + y, "this row has " + std::to_string(row.size())
                                         + " letters, the map is " + std::to_string(map.width)
                                         + " wide"};
            return false;
        }
        for (int x = 0; x < map.width; ++x) {
            if (!terrain_from_letter(row[static_cast<std::size_t>(x)],
                                     map.terrain[map.index(x, y)])) {
                failure = {line + 1 + y, std::string{"'"} + row[static_cast<std::size_t>(x)]
                                             + "' is not a terrain letter (f r R c)"};
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool read_heights(const std::vector<std::string>& rows, int line, MapDoc& map,
                                Failure& failure) {
    if (!check_grid(rows, map.height, line, failure)) {
        return false;
    }
    for (int y = 0; y < map.height; ++y) {
        std::istringstream cells{rows[static_cast<std::size_t>(y)]};
        std::string cell;
        int x = 0;
        while (cells >> cell) {
            int value = 0;
            if (x >= map.width || !parse_number(cell, value) || value < world::kMinHeight
                || value > world::kMaxHeight) {
                failure = {line + 1 + y, "'" + cell + "' is not a height from "
                                             + std::to_string(world::kMinHeight) + " to "
                                             + std::to_string(world::kMaxHeight)
                                             + ", or is one cell too many"};
                return false;
            }
            map.heights[map.index(x++, y)] = value;
        }
        if (x != map.width) {
            failure = {line + 1 + y, "this row has " + std::to_string(x) + " heights, the map is "
                                         + std::to_string(map.width) + " wide"};
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_solid(const std::vector<std::string>& rows, int line, MapDoc& map,
                              Failure& failure) {
    if (!check_grid(rows, map.height, line, failure)) {
        return false;
    }
    for (int y = 0; y < map.height; ++y) {
        const std::string& row = rows[static_cast<std::size_t>(y)];
        if (static_cast<int>(row.size()) != map.width) {
            failure
                = {line + 1 + y, "this row has " + std::to_string(row.size())
                                     + " tiles, the map is " + std::to_string(map.width) + " wide"};
            return false;
        }
        for (int x = 0; x < map.width; ++x) {
            const char c = row[static_cast<std::size_t>(x)];
            if (c != '#' && c != '.') {
                failure = {line + 1 + y, std::string{"'"} + c + "' is not # (solid) or . (open)"};
                return false;
            }
            map.solid[map.index(x, y)] = c == '#' ? 1 : 0;
        }
    }
    return true;
}

[[nodiscard]] bool read_volume(Fields& f, const MapDoc& map, MapVolume& volume, Failure& failure) {
    if (!f.integer("x", volume.x, 0, map.width - 1) || !f.integer("y", volume.y, 0, map.height - 1)
        || !f.boolean("turned", volume.turned) || !f.integer("base", volume.base, -4096, 4096)
        || !f.integer("top", volume.top, -4096, 4096)) {
        return false;
    }
    if (volume.top <= volume.base) {
        failure = {f.line_of("top"), "a volume's top must be above its base"};
        return false;
    }
    const Entry* entry = f.find("mask");
    if (entry == nullptr) {
        return false;
    }
    if (!entry->is_rows || entry->rows.empty()
        || static_cast<int>(entry->rows.size()) > kMaxMaskSide) {
        failure = {entry->line, "'mask' wants a \"\"\" block of 1 to 8 rows"};
        return false;
    }
    const std::size_t width = entry->rows.front().size();
    if (width == 0 || static_cast<int>(width) > kMaxMaskSide) {
        failure = {entry->line + 1, "a mask row is 1 to 8 tiles wide"};
        return false;
    }
    volume.mask_width = static_cast<int>(width);
    volume.mask_height = static_cast<int>(entry->rows.size());
    volume.mask_bits = 0;
    for (std::size_t ly = 0; ly < entry->rows.size(); ++ly) {
        const std::string& row = entry->rows[ly];
        if (row.size() != width) {
            failure = {entry->line + 1 + static_cast<int>(ly), "every mask row is the same width"};
            return false;
        }
        for (std::size_t lx = 0; lx < width; ++lx) {
            if (row[lx] != '#' && row[lx] != '.') {
                failure = {entry->line + 1 + static_cast<int>(ly), "a mask is # and . only"};
                return false;
            }
            if (row[lx] == '#') {
                volume.mask_bits |= std::uint64_t{1} << (ly * width + lx);
            }
        }
    }
    return true;
}

[[nodiscard]] MapParse fail(const Failure& failure) {
    MapParse parsed;
    parsed.error = failure.line > 0
                       ? "line " + std::to_string(failure.line) + ": " + failure.message
                       : failure.message;
    return parsed;
}

} // namespace

MapDoc generate_map(std::uint64_t seed, int size) {
    size = std::clamp(size, kMapMinSide, world::kChunkSize);
    MapDoc map;
    map.name = "seed " + std::to_string(seed);
    map.seed = seed;
    map.width = size;
    map.height = size;
    const auto cells = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    map.terrain.resize(cells);
    map.heights.resize(cells);
    map.solid.assign(cells, 0);
    const world::Chunk chunk = world::generate(seed, 0, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const world::Tile& tile = chunk.tiles.at(
                static_cast<std::size_t>(y) * static_cast<std::size_t>(world::kChunkSize)
                + static_cast<std::size_t>(x));
            map.terrain[map.index(x, y)] = tile.terrain;
            map.heights[map.index(x, y)] = tile.height;
        }
    }
    std::vector<world::Coord> spots;
    for (const world::Coord& spot : world::spawn_candidates(seed, 0, 0)) {
        if (spot.x < size && spot.y < size) {
            spots.push_back(spot);
        }
    }
    if (!spots.empty()) {
        map.spawns.push_back({"player", spots.front().x, spots.front().y});
    }
    if (spots.size() > 1) {
        map.spawns.push_back({"enemy", spots.back().x, spots.back().y});
    }
    return map;
}

std::string write_map(const MapDoc& map) {
    std::string out;
    out += "# PROTEKT map. Edit the rasters by hand; `protekt-map draw` shows what you made.\n";
    out += "# terrain: f flat, r rough, R ridge, c crater.  heights: integers, -512..512.\n";
    out += "# solid: # impassable, . open.  Row 0 is the north edge; column 0 the west.\n";
    out += "\n[map]\nname = " + quote(map.name) + "\nseed = " + std::to_string(map.seed)
           + "\nwidth = " + std::to_string(map.width) + "\nheight = " + std::to_string(map.height)
           + "\n";
    std::vector<std::string> rows;
    for (int y = 0; y < map.height; ++y) {
        std::string row;
        for (int x = 0; x < map.width; ++x) {
            row += terrain_letter(map.terrain[map.index(x, y)]);
        }
        rows.push_back(row);
    }
    out += "\n[terrain]\n";
    append_rows(out, rows);
    rows.clear();
    for (int y = 0; y < map.height; ++y) {
        std::string row;
        for (int x = 0; x < map.width; ++x) {
            const std::string cell = std::to_string(map.heights[map.index(x, y)]);
            row += x == 0 ? "" : " ";
            row += std::string(cell.size() < 4 ? 4 - cell.size() : 0, ' ') + cell;
        }
        rows.push_back(row);
    }
    out += "\n[height]\n";
    append_rows(out, rows);
    rows.clear();
    for (int y = 0; y < map.height; ++y) {
        std::string row;
        for (int x = 0; x < map.width; ++x) {
            row += map.solid[map.index(x, y)] != 0 ? '#' : '.';
        }
        rows.push_back(row);
    }
    out += "\n[solid]\n";
    append_rows(out, rows);
    for (const MapSpawn& spawn : map.spawns) {
        out += "\n[[spawn]]\nside = " + quote(spawn.side) + "\nx = " + std::to_string(spawn.x)
               + "\ny = " + std::to_string(spawn.y) + "\n";
    }
    for (const MapPlace& place : map.places) {
        out += "\n[[place]]\nname = " + quote(place.name) + "\nx = " + std::to_string(place.x)
               + "\ny = " + std::to_string(place.y) + "\nw = " + std::to_string(place.w)
               + "\nh = " + std::to_string(place.h) + "\n";
    }
    for (const MapVolume& v : map.volumes) {
        out += "\n[[volume]]\nx = " + std::to_string(v.x) + "\ny = " + std::to_string(v.y)
               + "\nturned = " + (v.turned ? "true" : "false")
               + "\nbase = " + std::to_string(v.base) + "\ntop = " + std::to_string(v.top) + "\n";
        rows.clear();
        for (int ly = 0; ly < v.mask_height; ++ly) {
            std::string row;
            for (int lx = 0; lx < v.mask_width; ++lx) {
                const auto bit = static_cast<unsigned>(ly * v.mask_width + lx);
                row += ((v.mask_bits >> bit) & 1U) != 0 ? '#' : '.';
            }
            rows.push_back(row);
        }
        out += "mask = \"\"\"\n";
        for (const std::string& row : rows) {
            out += row + "\n";
        }
        out += "\"\"\"\n";
    }
    return out;
}

MapParse parse_map(std::string_view text) {
    std::vector<Section> sections;
    Failure failure;
    if (!read_sections(text, sections, failure)) {
        return fail(failure);
    }
    MapParse result;
    MapDoc& map = result.map;
    const Section* header = nullptr;
    const Section* terrain = nullptr;
    const Section* heights = nullptr;
    const Section* solid = nullptr;
    for (const Section& section : sections) {
        const Section** slot = section.name == "map"       ? &header
                               : section.name == "terrain" ? &terrain
                               : section.name == "height"  ? &heights
                               : section.name == "solid"   ? &solid
                                                           : nullptr;
        if (section.name == "spawn" || section.name == "place" || section.name == "volume") {
            if (!section.array) {
                return fail({section.line, "[[" + section.name + "]] is a repeated table"});
            }
            continue;
        }
        if (slot == nullptr) {
            return fail({section.line, "unknown table '" + section.name + "'"});
        }
        if (section.array || *slot != nullptr) {
            return fail({section.line, "[" + section.name + "] is one table, written once"});
        }
        *slot = &section;
    }
    if (header == nullptr || terrain == nullptr || heights == nullptr) {
        return fail({0, "a map needs [map], [terrain] and [height]"});
    }
    {
        Fields f{*header, failure};
        if (!f.string("name", map.name) || !f.integer("width", map.width, kMapMinSide, kMapMaxSide)
            || !f.integer("height", map.height, kMapMinSide, kMapMaxSide)) {
            return fail(failure);
        }
        const Entry* seed_entry = f.find("seed");
        if (seed_entry == nullptr) {
            return fail(failure);
        }
        std::uint64_t parsed_seed = 0;
        if (!parse_number(seed_entry->text, parsed_seed)) {
            return fail({seed_entry->line, "'seed' wants an unsigned integer"});
        }
        map.seed = parsed_seed;
        if (!f.nothing_left()) {
            return fail(failure);
        }
    }
    const auto cells = static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height);
    map.terrain.assign(cells, Terrain::Flat);
    map.heights.assign(cells, 0);
    map.solid.assign(cells, 0);
    const auto raster = [&](const Section& section, auto reader) {
        Fields f{section, failure};
        const std::vector<std::string>* rows = f.rows();
        return rows != nullptr && f.nothing_left()
               && reader(*rows, f.line_of("rows"), map, failure);
    };
    if (!raster(*terrain, read_terrain) || !raster(*heights, read_heights)
        || (solid != nullptr && !raster(*solid, read_solid))) {
        return fail(failure);
    }
    for (const Section& section : sections) {
        Fields f{section, failure};
        if (section.name == "spawn") {
            MapSpawn spawn;
            if (!f.string("side", spawn.side) || !f.integer("x", spawn.x, 0, map.width - 1)
                || !f.integer("y", spawn.y, 0, map.height - 1) || !f.nothing_left()) {
                return fail(failure);
            }
            if (spawn.side != "player" && spawn.side != "enemy") {
                return fail({f.line_of("side"), "'side' is \"player\" or \"enemy\""});
            }
            map.spawns.push_back(spawn);
        } else if (section.name == "place") {
            MapPlace place;
            if (!f.string("name", place.name) || !f.integer("x", place.x, 0, map.width - 1)
                || !f.integer("y", place.y, 0, map.height - 1)
                || !f.integer("w", place.w, 1, map.width) || !f.integer("h", place.h, 1, map.height)
                || !f.nothing_left()) {
                return fail(failure);
            }
            if (place.x + place.w > map.width || place.y + place.h > map.height) {
                return fail({section.line, "a place runs off the map"});
            }
            map.places.push_back(place);
        } else if (section.name == "volume") {
            MapVolume volume;
            if (!read_volume(f, map, volume, failure) || !f.nothing_left()) {
                return fail(failure);
            }
            map.volumes.push_back(volume);
        }
    }
    result.ok = true;
    return result;
}

MapParse load_map_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        MapParse parsed;
        parsed.error = "cannot read " + path.string();
        return parsed;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse_map(text);
}

std::vector<sim::PlacedVolume> placed_volumes(const MapDoc& map) {
    std::vector<sim::PlacedVolume> out;
    for (const MapVolume& v : map.volumes) {
        sim::PlacedVolume placed;
        placed.x = v.x;
        placed.y = v.y;
        placed.turned = v.turned;
        placed.volume.extent = sim::SolidMask{v.mask_width, v.mask_height, v.mask_bits};
        placed.volume.base = sim::Fixed::from_raw(v.base * sim::Fixed::kOne);
        placed.volume.top = sim::Fixed::from_raw(v.top * sim::Fixed::kOne);
        out.push_back(placed);
    }
    return out;
}

} // namespace protekt::app
