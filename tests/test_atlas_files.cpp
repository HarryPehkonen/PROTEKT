// The atlas as real files (D2c): the generator, the loader, the override and fallback paths, and
// the half-texel inset. Every file written here goes to a temp directory, never into the repo.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "protekt/game/atlas.hpp"
#include "protekt/game/png.hpp"

namespace {

namespace fs = std::filesystem;
using protekt::game::AtlasManifest;
using protekt::game::kAtlasSide;
using protekt::game::kHalfTexel;
using protekt::game::kTileSide;
using protekt::game::load_atlas_assets;
using protekt::game::read_png_rgba;
using protekt::game::tile_rect;
using protekt::game::write_png_rgba;

/// A fresh empty directory under the system temp dir, removed on destruction.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path()
                / ("protekt-atlas-test-" + std::to_string(::getpid()) + "-"
                   + std::to_string(counter++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

/// An atlas whose every texel is `shade`, so it differs from the embedded one everywhere.
std::vector<std::uint8_t> flat_atlas(std::uint8_t shade) {
    return std::vector<std::uint8_t>(std::size_t{kAtlasSide} * kAtlasSide * 4U, shade);
}

TEST(AtlasGenerator, TwoRunsWriteByteIdenticalPngsAndItIsTheCommittedOne) {
    const TempDir dir;
    const fs::path a = dir.path() / "a.png";
    const fs::path b = dir.path() / "b.png";
    const fs::path c = dir.path() / "c.png";
    ASSERT_EQ(
        std::system(
            (std::string{PROTEKT_ATLAS_GEN} + " --out " + a.string() + " >/dev/null").c_str()),
        0);
    ASSERT_EQ(
        std::system(
            (std::string{PROTEKT_ATLAS_GEN} + " --out " + b.string() + " >/dev/null").c_str()),
        0);
    ASSERT_EQ(std::system(
                  (std::string{PROTEKT_ATLAS_GEN} + " --seed 7 --out " + c.string() + " >/dev/null")
                      .c_str()),
              0);
    const std::string bytes = slurp(a);
    EXPECT_GT(bytes.size(), std::size_t{0});
    EXPECT_EQ(bytes, slurp(b));
    // The seed picks the camo: another seed paints another atlas.
    EXPECT_NE(bytes, slurp(c));
    // The committed file is exactly what the tool writes: it is generated, never hand-made.
    EXPECT_EQ(bytes, slurp(fs::path{PROTEKT_SOURCE_DIR} / "assets" / "atlas.png"));
}

TEST(AtlasGenerator, ABadArgumentIsAUsageErrorAndWritesNothing) {
    const TempDir dir;
    const fs::path out = dir.path() / "never.png";
    EXPECT_NE(std::system((std::string{PROTEKT_ATLAS_GEN} + " --seed x --out " + out.string()
                           + " 2>/dev/null")
                              .c_str()),
              0);
    EXPECT_FALSE(fs::exists(out));
}

TEST(AtlasFiles, TheCommittedPngIs256SquareRgbaAndEqualsTheInMemoryTiles) {
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    ASSERT_TRUE(read_png_rgba(fs::path{PROTEKT_SOURCE_DIR} / "assets" / "atlas.png", rgba, w, h));
    EXPECT_EQ(w, std::uint32_t{256});
    EXPECT_EQ(h, std::uint32_t{256});
    EXPECT_EQ(rgba, protekt::game::atlas_pixels());
}

TEST(AtlasFiles, APngRoundTripsAndABadSizeOrFileIsRefused) {
    const TempDir dir;
    const fs::path file = dir.path() / "x.png";
    const auto pixels = flat_atlas(77);
    ASSERT_TRUE(write_png_rgba(file, pixels, kAtlasSide, kAtlasSide));
    std::vector<std::uint8_t> back;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    ASSERT_TRUE(read_png_rgba(file, back, w, h));
    EXPECT_EQ(back, pixels);
    EXPECT_FALSE(write_png_rgba(file, pixels, kAtlasSide, kAtlasSide - 1));
    EXPECT_FALSE(read_png_rgba(dir.path() / "missing.png", back, w, h));
    spit(dir.path() / "junk.png", "not a png at all");
    EXPECT_FALSE(read_png_rgba(dir.path() / "junk.png", back, w, h));
}

TEST(AtlasLoading, ADifferentPngInTheSearchDirectoryIsTheOneThatGetsUploaded) {
    const TempDir dir;
    const auto custom = flat_atlas(9);
    ASSERT_TRUE(write_png_rgba(dir.path() / "atlas.png", custom, kAtlasSide, kAtlasSide));
    std::ostringstream log;
    const std::vector<fs::path> dirs{dir.path()};
    const auto assets = load_atlas_assets(dirs, log);
    EXPECT_EQ(assets.pixels, custom);
    EXPECT_NE(assets.pixels, protekt::game::atlas_pixels());
    EXPECT_EQ(assets.pixels_source, (dir.path() / "atlas.png").string());
}

TEST(AtlasLoading, TheWorkingDirectorysAssetsFolderIsSearchedFirst) {
    const TempDir dir;
    fs::create_directories(dir.path() / "assets");
    const auto custom = flat_atlas(33);
    ASSERT_TRUE(
        write_png_rgba(dir.path() / "assets" / "atlas.png", custom, kAtlasSide, kAtlasSide));
    const fs::path before = fs::current_path();
    fs::current_path(dir.path());
    std::ostringstream log;
    const auto dirs = protekt::game::default_asset_dirs();
    const auto assets = load_atlas_assets(dirs, log);
    fs::current_path(before);
    ASSERT_FALSE(dirs.empty());
    EXPECT_EQ(dirs.front(), fs::path{"assets"});
    EXPECT_EQ(assets.pixels, custom);
}

TEST(AtlasLoading, WithNoFilesTheEmbeddedAtlasAndManifestAreUsedAndOneLineSaysSo) {
    const TempDir dir;
    std::ostringstream log;
    const std::vector<fs::path> dirs{dir.path()};
    const auto assets = load_atlas_assets(dirs, log);
    EXPECT_EQ(assets.pixels, protekt::game::atlas_pixels());
    EXPECT_EQ(assets.pixels_source, "embedded");
    EXPECT_EQ(assets.manifest_source, "embedded");
    EXPECT_EQ(assets.manifest.size(), protekt::game::embedded_atlas_manifest().size());
    EXPECT_EQ(assets.manifest.tile("tank_barrel"), 3);
    EXPECT_NE(log.str().find("embedded atlas"), std::string::npos);
}

TEST(AtlasLoading, AWrongSizedOrCorruptPngFallsBackInsteadOfFailing) {
    const TempDir dir;
    const std::vector<std::uint8_t> small(std::size_t{8} * 8 * 4, 5);
    ASSERT_TRUE(write_png_rgba(dir.path() / "atlas.png", small, 8, 8));
    const std::vector<fs::path> dirs{dir.path()};
    std::ostringstream log;
    EXPECT_EQ(load_atlas_assets(dirs, log).pixels, protekt::game::atlas_pixels());
    spit(dir.path() / "atlas.png", "garbage");
    EXPECT_EQ(load_atlas_assets(dirs, log).pixels, protekt::game::atlas_pixels());
}

TEST(AtlasLoading, TheManifestFileOverridesTheEmbeddedManifest) {
    const TempDir dir;
    spit(dir.path() / "atlas_manifest.txt", "tank_hull 9\nbrand_new_part 12\n");
    std::ostringstream log;
    const std::vector<fs::path> dirs{dir.path()};
    const auto assets = load_atlas_assets(dirs, log);
    EXPECT_EQ(assets.manifest.tile("tank_hull"), 9);
    EXPECT_EQ(assets.manifest.tile("brand_new_part"), 12);
    EXPECT_EQ(assets.manifest.tile("tank_barrel"), protekt::game::kFallbackTile);
    EXPECT_EQ(assets.manifest_source, (dir.path() / "atlas_manifest.txt").string());
    // The manifest file and the PNG are independent: the pixels here are still the embedded ones.
    EXPECT_EQ(assets.pixels_source, "embedded");
}

TEST(AtlasLoading, AMissingOrEmptyManifestFileFallsBackToTheEmbeddedOne) {
    const TempDir dir;
    const std::vector<fs::path> dirs{dir.path()};
    std::ostringstream log;
    EXPECT_EQ(load_atlas_assets(dirs, log).manifest_source, "embedded");
    spit(dir.path() / "atlas_manifest.txt", "# nothing valid\nnot a line\n");
    const auto assets = load_atlas_assets(dirs, log);
    EXPECT_EQ(assets.manifest_source, "embedded");
    EXPECT_EQ(assets.manifest.tile("tank_hull"), 7);
    EXPECT_EQ(assets.manifest.size(), std::size_t{10});
}

TEST(AtlasLoading, ALaterDirectoryIsUsedWhenAnEarlierOneHasNothing) {
    const TempDir empty;
    const TempDir full;
    const auto custom = flat_atlas(200);
    ASSERT_TRUE(write_png_rgba(full.path() / "atlas.png", custom, kAtlasSide, kAtlasSide));
    const std::vector<fs::path> dirs{empty.path(), full.path()};
    std::ostringstream log;
    EXPECT_EQ(load_atlas_assets(dirs, log).pixels, custom);
}

TEST(AtlasLoading, InstallingAManifestChangesWhatTheSceneReadsAndCanBeUndone) {
    protekt::game::install_atlas_manifest(AtlasManifest::parse("tank_hull 5\n"));
    EXPECT_EQ(protekt::game::atlas_manifest().tile("tank_hull"), 5);
    protekt::game::install_atlas_manifest(protekt::game::embedded_atlas_manifest());
    EXPECT_EQ(protekt::game::atlas_manifest().tile("tank_hull"), 7);
}

TEST(AtlasInset, ATileRectIsTheTileShrunkByHalfATexelPerSide) {
    EXPECT_EQ(kHalfTexel, 1.0F / 512.0F);
    for (int index = 0; index < protekt::game::kTileCount; ++index) {
        const auto col = static_cast<std::uint32_t>(index) % protekt::game::kTilesPerRow;
        const auto row = static_cast<std::uint32_t>(index) / protekt::game::kTilesPerRow;
        const auto side = static_cast<float>(kAtlasSide);
        const auto texel_x0 = static_cast<float>(col * kTileSide);
        const auto texel_y0 = static_cast<float>(row * kTileSide);
        const auto tile = static_cast<float>(kTileSide);
        const auto r = tile_rect(index);
        EXPECT_EQ(r.u0, (texel_x0 + 0.5F) / side) << index;
        EXPECT_EQ(r.v0, (texel_y0 + 0.5F) / side) << index;
        EXPECT_EQ(r.u1, (texel_x0 + tile - 0.5F) / side) << index;
        EXPECT_EQ(r.v1, (texel_y0 + tile - 0.5F) / side) << index;
    }
}

TEST(AtlasInset, NoUvOfATileLiesOutsideItsOwnTexelRange) {
    for (int index = 0; index < protekt::game::kTileCount; ++index) {
        const auto col = static_cast<std::uint32_t>(index) % protekt::game::kTilesPerRow;
        const auto row = static_cast<std::uint32_t>(index) / protekt::game::kTilesPerRow;
        const auto r = tile_rect(index);
        const auto side = static_cast<float>(kAtlasSide);
        for (const float u : {r.u0, r.u1}) {
            const auto texel = static_cast<std::uint32_t>(u * side); // NEAREST fetch
            EXPECT_EQ(texel / kTileSide, col) << index;
        }
        for (const float v : {r.v0, r.v1}) {
            const auto texel = static_cast<std::uint32_t>(v * side);
            EXPECT_EQ(texel / kTileSide, row) << index;
        }
    }
}

} // namespace
