// CLIMATE (slice A2): weather as a property of the MAP. A map's header says what its ground and air
// are; the scene reads it. A LOOK and only a look: nothing here may reach the sim, and sand and ice
// must NOT make anything slippery (Harri: "Let's leave the slipperiness alone for now.  I haven't
// noticed any difference in BZ2.").
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "protekt/app/map_file.hpp"
#include "protekt/game/climate.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::build_scene;
using protekt::game::Climate;
using protekt::game::climate_from_name;
using protekt::game::climate_name;
using protekt::game::fog_colour;
using protekt::game::fog_far_metres;
using protekt::game::kClimateCount;
using protekt::game::kSkyHorizonRing;
using protekt::game::kSkyRingColours;
using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::game::sky_ring_colour;
using protekt::sim::Side;
using protekt::sim::Spawn;
using protekt::sim::UnitKind;

constexpr std::array<Climate, kClimateCount> kAll{Climate::Temperate, Climate::Sand, Climate::Ice,
                                                  Climate::Regolith};
constexpr int kRadius = 20;

protekt::sim::Sim battle() {
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 48, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 52, 54});
    return protekt::sim::Sim::create(scenario);
}

SceneOptions rig(Climate climate) {
    SceneOptions o;
    o.seed = 42;
    o.climate = climate;
    o.radius_tiles = kRadius;
    o.camera.x = 100.0F;
    o.camera.y = 8.0F;
    o.camera.z = 100.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

std::size_t terrain_vertices() {
    const auto side = static_cast<std::size_t>(kRadius) * 2U + 2U;
    return side * side * 6U;
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string good_map(const std::string& climate_line) {
    return "[map]\nname = \"t\"\nseed = 1\nwidth = 2\nheight = 2\n" + climate_line
           + "\n[terrain]\nrows = \"\"\"\nff\nff\n\"\"\"\n[height]\nrows = \"\"\"\n   0    0\n   0 "
             "   0\n\"\"\"\n";
}

// What the map says -----------------------------------------------------------------------------

TEST(Climate, EveryClimateHasItsOwnNameAndTheNameRoundTrips) {
    std::vector<std::string> names;
    for (const Climate c : kAll) {
        const auto name = std::string{climate_name(c)};
        names.push_back(name);
        ASSERT_TRUE(climate_from_name(name).has_value()) << name;
        EXPECT_EQ(climate_from_name(name), std::optional<Climate>{c});
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end()) << "names are distinct";
    EXPECT_EQ(names, (std::vector<std::string>{"ice", "regolith", "sand", "temperate"}));
}

TEST(Climate, TheHeaderKeyIsReadFromTheMap) {
    for (const Climate c : kAll) {
        const auto parsed = protekt::app::parse_map(
            good_map("climate = \"" + std::string{climate_name(c)} + "\""));
        ASSERT_TRUE(parsed.ok) << parsed.error;
        EXPECT_EQ(parsed.map.climate, c);
    }
}

TEST(Climate, NoKeyIsTheTemperateLookEveryOlderMapHad) {
    const auto parsed = protekt::app::parse_map(good_map(""));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.map.climate, Climate::Temperate);
}

TEST(Climate, AnUnknownClimateIsRefusedByNameAndLineNotFallenBackTo) {
    // The house rule, in the game's own source: "a bad data file must never quietly play a
    // different game". The header is lines 1-6 of the text below; the bad value is on line 6.
    for (const char* bad : {"climate = \"swamp\"", "climate = \"Sand\"", "climate = \"\""}) {
        const auto parsed = protekt::app::parse_map(good_map(bad));
        EXPECT_FALSE(parsed.ok) << bad;
        EXPECT_NE(parsed.error.find("line 6"), std::string::npos) << parsed.error;
        EXPECT_NE(parsed.error.find("climate"), std::string::npos) << parsed.error;
    }
    const auto swamp = protekt::app::parse_map(good_map("climate = \"swamp\""));
    EXPECT_NE(swamp.error.find("swamp"), std::string::npos) << "the bad value is named";
    EXPECT_NE(swamp.error.find("regolith"), std::string::npos) << "and so are the good ones";
    const auto unquoted = protekt::app::parse_map(good_map("climate = sand"));
    EXPECT_FALSE(unquoted.ok) << "a climate is a quoted string like every other header string";
}

TEST(Climate, AMapWritesItsClimateAndReadsItBackByteIdentically) {
    for (const Climate c : kAll) {
        auto doc = protekt::app::generate_map(7, 16);
        doc.climate = c;
        const std::string text = protekt::app::write_map(doc);
        EXPECT_EQ(text, protekt::app::write_map(doc)) << "two writes are byte-identical";
        const auto back = protekt::app::parse_map(text);
        ASSERT_TRUE(back.ok) << back.error;
        EXPECT_EQ(back.map.climate, c);
        EXPECT_EQ(protekt::app::write_map(back.map), text);
    }
}

TEST(Climate, TheShippedMapSaysItsClimateInItsHeader) {
    const std::string text
        = slurp(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    EXPECT_NE(text.find("\nclimate = \"temperate\"\n"), std::string::npos);
}

// What the renderer consumes ----------------------------------------------------------------------

TEST(Climate, TemperateIsTheOriginalLookExactly) {
    EXPECT_EQ(fog_colour(Climate::Temperate), kSkyRingColours.at(kSkyHorizonRing));
    for (std::size_t ring = 0; ring < protekt::game::kSkyRings; ++ring) {
        EXPECT_EQ(sky_ring_colour(Climate::Temperate, ring), kSkyRingColours.at(ring)) << ring;
    }
    EXPECT_EQ(fog_far_metres(kRadius, Climate::Temperate), fog_far_metres(kRadius));
    for (const auto& gain : protekt::game::climate_look(Climate::Temperate).ground_gain) {
        EXPECT_EQ(gain, (protekt::math::Vec3{1.0F, 1.0F, 1.0F}));
    }
}

TEST(Climate, TheFogColourIsTheSkysHorizonUnderEveryClimate) {
    for (const Climate c : kAll) {
        EXPECT_EQ(fog_colour(c), sky_ring_colour(c, kSkyHorizonRing))
            << climate_name(c) << ": or the rim ends in a seam against its own sky";
    }
}

TEST(Climate, TwoClimatesGiveTheRendererDifferentValues) {
    const auto sim = battle();
    std::vector<Scene> scenes;
    for (const Climate c : kAll) {
        scenes.push_back(build_scene(sim, rig(c)));
        ASSERT_TRUE(scenes.back().fog_colour.has_value()) << climate_name(c);
        const std::optional<std::array<float, 3>> expected{fog_colour(c)};
        EXPECT_EQ(scenes.back().fog_colour, expected) << "the scene carries it to the renderer";
    }
    for (std::size_t a = 0; a < kAll.size(); ++a) {
        for (std::size_t b = a + 1; b < kAll.size(); ++b) {
            const std::string pair = std::string{climate_name(kAll.at(a))} + " vs "
                                     + std::string{climate_name(kAll.at(b))};
            EXPECT_NE(scenes.at(a).fog_colour, scenes.at(b).fog_colour) << pair << ": fog colour";
            // The ground paint: some terrain vertex differs.
            bool ground = false;
            bool sky = false;
            for (std::size_t i = 0; i < terrain_vertices(); ++i) {
                ground
                    = ground
                      || scenes.at(a).vertices.at(i).colour != scenes.at(b).vertices.at(i).colour;
            }
            for (std::size_t i = terrain_vertices();
                 i < terrain_vertices() + protekt::game::kSkyVertexCount; ++i) {
                sky = sky
                      || scenes.at(a).vertices.at(i).colour != scenes.at(b).vertices.at(i).colour;
            }
            EXPECT_TRUE(ground) << pair << ": ground paint";
            EXPECT_TRUE(sky) << pair << ": sky";
        }
    }
}

TEST(Climate, AHazyPlaceAndAClearPlaceDifferBecauseTheMapSaysSo) {
    // Fog strength is a climate value: same distance, same radius, different amount.
    const float clear = fog_far_metres(kRadius, Climate::Temperate);
    for (const Climate hazy : {Climate::Sand, Climate::Ice}) {
        const float far = fog_far_metres(kRadius, hazy);
        EXPECT_LT(far, clear) << climate_name(hazy) << " is hazier than clear";
        EXPECT_GT(far, protekt::game::kFogNearMetres);
        EXPECT_GT(protekt::game::fog_amount(20.0F, far), protekt::game::fog_amount(20.0F, clear));
    }
    EXPECT_EQ(fog_far_metres(kRadius, Climate::Regolith), clear) << "airless: nothing to haze";
    // And the scene stores it per vertex, from the map's climate.
    const auto sim = battle();
    const Scene sand = build_scene(sim, rig(Climate::Sand));
    const Scene temperate = build_scene(sim, rig(Climate::Temperate));
    bool hazier = false;
    for (std::size_t i = 0; i < terrain_vertices(); ++i) {
        EXPECT_GE(sand.vertices.at(i).fog, temperate.vertices.at(i).fog);
        hazier = hazier || sand.vertices.at(i).fog > temperate.vertices.at(i).fog;
    }
    EXPECT_TRUE(hazier);
}

TEST(Climate, TheSameClimateOnTheSameSeedIsByteIdenticalTwice) {
    const auto sim = battle();
    for (const Climate c : kAll) {
        const Scene first = build_scene(sim, rig(c));
        const Scene second = build_scene(sim, rig(c));
        ASSERT_EQ(first.vertices.size(), second.vertices.size());
        EXPECT_EQ(first.fog_colour, second.fog_colour);
        for (std::size_t i = 0; i < first.vertices.size(); ++i) {
            const auto& a = first.vertices.at(i);
            const auto& b = second.vertices.at(i);
            ASSERT_TRUE(a.position == b.position && a.colour == b.colour && a.alpha == b.alpha
                        && a.u == b.u && a.v == b.v && a.fog == b.fog)
                << climate_name(c) << " vertex " << i;
        }
    }
}

// What a climate must NOT touch
// --------------------------------------------------------------------

TEST(Climate, AClimateNeverMovesTheSimStateHash) {
    // The number recorded on the tree BEFORE climates existed (the stub that ignored them), and
    // checked again after every build, under every climate.
    constexpr std::uint64_t kBefore = 7945841677604712876ULL;
    auto sim = battle();
    const std::uint64_t before = sim.state_hash();
    EXPECT_EQ(before, kBefore);
    for (const Climate c : kAll) {
        (void)build_scene(sim, rig(c));
        EXPECT_EQ(sim.state_hash(), before) << climate_name(c);
    }
    for (int i = 0; i < 120; ++i) {
        sim.step();
    }
    // The same battle played with climates being built every tick lands where one played blind
    // does: the look has no road into the sim.
    auto watched = battle();
    for (int i = 0; i < 120; ++i) {
        (void)build_scene(watched, rig(kAll.at(static_cast<std::size_t>(i) % kAll.size())));
        watched.step();
    }
    EXPECT_EQ(watched.state_hash(), sim.state_hash());
}

TEST(Climate, SandAndIceHaveNoMovementEffectAndTheSimNeverReadsAClimate) {
    // THE TRAP, SHUT. Harri ruled sand and ice LOOKS ONLY: "Let's leave the slipperiness alone for
    // now.  I haven't noticed any difference in BZ2."  A later session that adds a movement rule
    // for them has to teach the sim (or the world it reads) what a climate is -- and fails here.
    // It is a proposal, not a change: write it up and ask.
    namespace fs = std::filesystem;
    const fs::path root{PROTEKT_SOURCE_DIR};
    std::size_t files = 0;
    for (const char* dir :
         {"src/sim", "include/protekt/sim", "src/world", "include/protekt/world"}) {
        ASSERT_TRUE(fs::exists(root / dir)) << dir;
        for (const auto& entry : fs::recursive_directory_iterator(root / dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            ++files;
            std::string text = slurp(entry.path());
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            for (const char* word : {"climate", "slipper", "regolith"}) {
                EXPECT_EQ(text.find(word), std::string::npos)
                    << entry.path() << " mentions '" << word << "'";
            }
        }
    }
    EXPECT_GT(files, 10U);
}

TEST(Climate, NothingPurelyVisualIsOnTheHud) {
    // "Not to radar, unless there is some radar-blocking device." The radar and the rest of the HUD
    // are drawn after `world_vertex_count`, and no climate touches any of it.
    auto sim = battle();
    auto options = [&](Climate c) {
        SceneOptions o = rig(c);
        o.camera.x = 50.0F * protekt::game::kMetresPerTile;
        o.camera.z = 38.0F * protekt::game::kMetresPerTile;
        o.camera.y = 14.0F;
        o.viewer_unit = sim.units().front().id;
        o.extent = {800, 450};
        o.show_ledger = true;
        return o;
    };
    const Scene base = build_scene(sim, options(Climate::Temperate));
    ASSERT_LT(base.world_vertex_count, base.vertices.size())
        << "no HUD was drawn: the rig is wrong";
    for (const Climate c : {Climate::Sand, Climate::Ice, Climate::Regolith}) {
        const Scene other = build_scene(sim, options(c));
        ASSERT_EQ(other.vertices.size(), base.vertices.size()) << climate_name(c);
        ASSERT_EQ(other.world_vertex_count, base.world_vertex_count);
        for (std::size_t i = base.world_vertex_count; i < base.vertices.size(); ++i) {
            const auto& a = base.vertices.at(i);
            const auto& b = other.vertices.at(i);
            ASSERT_TRUE(a.position == b.position && a.colour == b.colour && a.alpha == b.alpha)
                << climate_name(c) << " HUD vertex " << i;
        }
    }
}

} // namespace
