// The Instant Action composition as data: the built-in default is today's skirmish unchanged, and
// the TOML subset that replaces it is read strictly -- a bad file names its line, and nothing is
// skipped, guessed or defaulted.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "protekt/app/scenario_toml.hpp"
#include "protekt/sim/scenario.hpp"

namespace {

using namespace protekt::sim;
using protekt::app::load_scenario_data;
using protekt::app::load_scenario_file;
using protekt::app::ScenarioParse;

constexpr const char* kGroups = R"(# Two groups.
[[group]]
kind = "tank"
count = 3
dx = 8
dy = 2

[[group]]
kind = "turret"
count = 2
dx = 5
dy = -3
)";

/// True when `error` names the line and says `what`.
::testing::AssertionResult names(const ScenarioParse& parse, const std::string& line,
                                 const std::string& what) {
    if (parse.ok) {
        return ::testing::AssertionFailure() << "parsed, but should have been refused";
    }
    if (parse.error.find(line) == std::string::npos
        || parse.error.find(what) == std::string::npos) {
        return ::testing::AssertionFailure()
               << "error '" << parse.error << "' lacks '" << line << "' or '" << what << "'";
    }
    return ::testing::AssertionSuccess();
}

// --- the built-in default ----------------------------------------------------------------------

TEST(ScenarioData, DefaultIsUnchanged) {
    // Today's roster and offsets, entry for entry: tanks lead, the hover tank, the factory at the
    // base centre, the turrets flank it, the mortar bike and the rocket tank behind, the scavenger
    // beside the recycler, and the recycler last (HAULER: the scavenger spawns, so the economy can
    // be played; it sits before the recycler to keep RB1b's "recycler is the last id" rule).
    const std::vector<Group> want{
        {UnitKind::Tank, 1, 8, 2},         {UnitKind::Tank, 1, 8, 8},
        {UnitKind::Tank, 1, 2, 8},         {UnitKind::HoverTank, 1, 5, 5},
        {UnitKind::Factory, 1, 0, 0},      {UnitKind::Turret, 1, 5, -3},
        {UnitKind::Turret, 1, -3, 5},      {UnitKind::MortarBike, 1, -4, -1},
        {UnitKind::RocketTank, 1, -1, -4}, {UnitKind::Scavenger, 1, 1, -3},
        {UnitKind::Recycler, 1, -3, -3},
    };
    const ScenarioData data = default_data();
    ASSERT_EQ(data.groups.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(data.groups.at(i).kind, want.at(i).kind) << "group " << i;
        EXPECT_EQ(data.groups.at(i).count, want.at(i).count) << "group " << i;
        EXPECT_EQ(data.groups.at(i).dx, want.at(i).dx) << "group " << i;
        EXPECT_EQ(data.groups.at(i).dy, want.at(i).dy) << "group " << i;
    }
}

TEST(ScenarioData, SkirmishWithoutDataIsSkirmishWithTheDefault) {
    for (const std::uint64_t seed : {1ULL, 7ULL, 42ULL, 777ULL}) {
        const Scenario a = skirmish(seed);
        const Scenario b = skirmish(seed, default_data());
        EXPECT_EQ(a.seed, b.seed);
        ASSERT_EQ(a.spawns.size(), 22U) << "seed " << seed;
        ASSERT_EQ(a.spawns.size(), b.spawns.size()) << "seed " << seed;
        for (std::size_t i = 0; i < a.spawns.size(); ++i) {
            EXPECT_EQ(a.spawns.at(i).kind, b.spawns.at(i).kind) << "seed " << seed << " #" << i;
            EXPECT_EQ(a.spawns.at(i).side, b.spawns.at(i).side) << "seed " << seed << " #" << i;
            EXPECT_EQ(a.spawns.at(i).tile_x, b.spawns.at(i).tile_x) << "seed " << seed << " #" << i;
            EXPECT_EQ(a.spawns.at(i).tile_y, b.spawns.at(i).tile_y) << "seed " << seed << " #" << i;
        }
    }
}

TEST(ScenarioData, TheRosterExpandsEachGroupToItsCountInOrder) {
    ScenarioData data;
    data.groups = {
        {UnitKind::RocketTank, 2, 0, 0}, {UnitKind::Tank, 3, 1, 1}, {UnitKind::Factory, 1, 2, 2}};
    const std::vector<UnitKind> want{UnitKind::RocketTank, UnitKind::RocketTank, UnitKind::Tank,
                                     UnitKind::Tank,       UnitKind::Tank,       UnitKind::Factory};
    EXPECT_EQ(side_roster(data, Side::Player), want);
    EXPECT_EQ(side_roster(data, Side::Enemy), want);
}

// --- reading the TOML subset -------------------------------------------------------------------

TEST(ScenarioToml, ParsesGroupsKindsCountsAndOrder) {
    const ScenarioParse parse = load_scenario_data(kGroups);
    ASSERT_TRUE(parse.ok) << parse.error;
    ASSERT_EQ(parse.data.groups.size(), 2U);
    EXPECT_EQ(parse.data.groups.at(0).kind, UnitKind::Tank);
    EXPECT_EQ(parse.data.groups.at(0).count, 3);
    EXPECT_EQ(parse.data.groups.at(0).dx, 8);
    EXPECT_EQ(parse.data.groups.at(0).dy, 2);
    EXPECT_EQ(parse.data.groups.at(1).kind, UnitKind::Turret);
    EXPECT_EQ(parse.data.groups.at(1).count, 2);
    EXPECT_EQ(parse.data.groups.at(1).dx, 5);
    EXPECT_EQ(parse.data.groups.at(1).dy, -3);
}

TEST(ScenarioToml, EveryKindIsNamedInLowerCaseWithUnderscores) {
    struct Named {
        const char* name;
        UnitKind kind;
    };
    const std::array<Named, 9> kinds{{{"tank", UnitKind::Tank},
                                      {"turret", UnitKind::Turret},
                                      {"factory", UnitKind::Factory},
                                      {"hover_tank", UnitKind::HoverTank},
                                      {"mortar_bike", UnitKind::MortarBike},
                                      {"rocket_tank", UnitKind::RocketTank},
                                      {"scavenger", UnitKind::Scavenger},
                                      {"recycler", UnitKind::Recycler},
                                      {"deployed_recycler", UnitKind::DeployedRecycler}}};
    for (const Named& entry : kinds) {
        const std::string text
            = std::string("[[group]]\nkind = \"") + entry.name + "\"\ncount = 1\ndx = 0\ndy = 0\n";
        const ScenarioParse parse = load_scenario_data(text);
        ASSERT_TRUE(parse.ok) << entry.name << ": " << parse.error;
        EXPECT_EQ(parse.data.groups.at(0).kind, entry.kind) << entry.name;
    }
}

TEST(ScenarioToml, TheSupportVehiclesAreNamedMortarBikeAndRocketTank) {
    for (const auto& [name, kind] : {std::pair{"mortar_bike", UnitKind::MortarBike},
                                     std::pair{"rocket_tank", UnitKind::RocketTank}}) {
        const std::string text
            = std::string("[[group]]\nkind = \"") + name + "\"\ncount = 1\ndx = 0\ndy = 0\n";
        const ScenarioParse parse = load_scenario_data(text);
        ASSERT_TRUE(parse.ok) << name << ": " << parse.error;
        EXPECT_EQ(parse.data.groups.at(0).kind, kind) << name;
    }
}

TEST(ScenarioToml, TheWeaponNamesAreNoLongerVehicleKinds) {
    // No alias: a scenario written before the rename fails loudly, naming the line and the word.
    for (const char* old_name : {"mortar", "rocket", "mortarbike", "rockettank"}) {
        const std::string text = std::string("[[group]]\n# c\nkind = \"") + old_name
                                 + "\"\ncount = 1\ndx = 0\ndy = 0\n";
        const ScenarioParse parse = load_scenario_data(text);
        EXPECT_FALSE(parse.ok) << old_name;
        EXPECT_TRUE(names(parse, "line 3", old_name)) << old_name << ": " << parse.error;
    }
}

TEST(ScenarioToml, IgnoresCommentsAndBlankLines) {
    const ScenarioParse parse
        = load_scenario_data("\n# header\n   # indented comment\n\n[[group]] # the first\n\tkind = "
                             "\"mortar_bike\"  # a kind\n"
                             "count=4\r\n\n  dx =   -7 \ndy = 0#tight\n\n\n# trailer");
    ASSERT_TRUE(parse.ok) << parse.error;
    ASSERT_EQ(parse.data.groups.size(), 1U);
    EXPECT_EQ(parse.data.groups.at(0).kind, UnitKind::MortarBike);
    EXPECT_EQ(parse.data.groups.at(0).count, 4);
    EXPECT_EQ(parse.data.groups.at(0).dx, -7);
    EXPECT_EQ(parse.data.groups.at(0).dy, 0);
}

TEST(ScenarioToml, UnknownKindNamesTheLine) {
    const ScenarioParse parse
        = load_scenario_data("[[group]]\n# c\nkind = \"hovertank\"\ncount = 1\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 3", "hovertank"));
}

TEST(ScenarioToml, UnknownKeyNamesTheLine) {
    const ScenarioParse parse
        = load_scenario_data("[[group]]\nkind = \"tank\"\ncount = 1\nspeed = 3\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 4", "speed"));
}

TEST(ScenarioToml, UnknownTableAndNestedTableNameTheLine) {
    EXPECT_TRUE(names(load_scenario_data("\n[squad]\n"), "line 2", "squad"));
    EXPECT_TRUE(names(load_scenario_data("[[group]]\n[group.extra]\n"), "line 2", "group.extra"));
    EXPECT_TRUE(names(load_scenario_data("[group]\n"), "line 1", "[group]"));
}

TEST(ScenarioToml, MissingKeyNamesTheLine) {
    // The group opens on line 3 and never says dy: the line named is the group's own.
    const ScenarioParse parse
        = load_scenario_data("# c\n\n[[group]]\nkind = \"tank\"\ncount = 1\ndx = 0\n");
    EXPECT_TRUE(names(parse, "line 3", "dy"));
    // The same, when another group follows.
    const ScenarioParse middle = load_scenario_data(
        "[[group]]\nkind = \"tank\"\ncount = 1\ndx = 0\n[[group]]\nkind = \"tank\"\ncount = 1\n"
        "dx = 0\ndy = 0\n");
    EXPECT_TRUE(names(middle, "line 1", "dy"));
}

TEST(ScenarioToml, DuplicateKeyNamesTheLine) {
    const ScenarioParse parse
        = load_scenario_data("[[group]]\nkind = \"tank\"\ncount = 1\ncount = 2\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 4", "count"));
}

TEST(ScenarioToml, NonIntegerCountNamesTheLine) {
    for (const char* bad : {"3.5", "\"3\"", "three", "+3", "", "0x10", "[3]", "99999999999"}) {
        const ScenarioParse parse = load_scenario_data(
            std::string("[[group]]\nkind = \"tank\"\ncount = ") + bad + "\ndx = 0\ndy = 0\n");
        EXPECT_TRUE(names(parse, "line 3", "count")) << "value '" << bad << "'";
    }
}

TEST(ScenarioToml, KindWantsAQuotedString) {
    const ScenarioParse parse
        = load_scenario_data("[[group]]\nkind = tank\ncount = 1\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 2", "kind"));
}

TEST(ScenarioToml, ALineWithoutAnEqualsSignNamesTheLine) {
    EXPECT_TRUE(names(load_scenario_data("[[group]]\nkind \"tank\"\n"), "line 2", "="));
    EXPECT_TRUE(names(load_scenario_data("kind = \"tank\"\n"), "line 1", "[[group]]"));
}

TEST(ScenarioToml, TrailingTextAfterAValueNamesTheLine) {
    const ScenarioParse parse
        = load_scenario_data("[[group]]\nkind = \"tank\" extra\ncount = 1\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 2", "kind"));
}

TEST(ScenarioToml, EmptyFileIsAnError) {
    for (const char* text : {"", "\n\n", "# only a comment\n\n"}) {
        const ScenarioParse parse = load_scenario_data(text);
        EXPECT_FALSE(parse.ok);
        EXPECT_NE(parse.error.find("[[group]]"), std::string::npos) << parse.error;
        EXPECT_NE(parse.error.find("64"), std::string::npos) << "states the bound: " << parse.error;
    }
}

TEST(ScenarioToml, WeaponFieldIsRejectedWithTheReason) {
    const ScenarioParse parse = load_scenario_data(
        "[[group]]\nkind = \"tank\"\ncount = 3\nweapon = \"at_stabber\"\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 4", "weapon"));
    EXPECT_NE(parse.error.find("property of the kind"), std::string::npos) << parse.error;
    EXPECT_EQ(parse.error.find("unknown key"), std::string::npos) << "not a misspelling";
}

TEST(ScenarioToml, KeyFieldIsRejectedWithTheReason) {
    const ScenarioParse parse = load_scenario_data(
        "[[group]]\nkey = \"F1\"\nkind = \"tank\"\ncount = 3\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 2", "key"));
    EXPECT_NE(parse.error.find("order"), std::string::npos) << parse.error;
    EXPECT_EQ(parse.error.find("unknown key"), std::string::npos) << "not a misspelling";
}

TEST(ScenarioToml, CountOutsideItsBoundsNamesTheLineAndTheBound) {
    for (const char* bad : {"0", "-1", "65"}) {
        const ScenarioParse parse = load_scenario_data(
            std::string("[[group]]\nkind = \"tank\"\ncount = ") + bad + "\ndx = 0\ndy = 0\n");
        EXPECT_TRUE(names(parse, "line 3", "1..64")) << "count " << bad;
    }
    EXPECT_TRUE(load_scenario_data("[[group]]\nkind = \"tank\"\ncount = 64\ndx = 0\ndy = 0\n").ok);
    EXPECT_TRUE(load_scenario_data("[[group]]\nkind = \"tank\"\ncount = 1\ndx = 0\ndy = 0\n").ok);
}

TEST(ScenarioToml, OffsetOutsideItsBoundsNamesTheLineAndTheBound) {
    for (const char* bad : {"33", "-33", "1000"}) {
        const ScenarioParse dx = load_scenario_data(
            std::string("[[group]]\nkind = \"tank\"\ncount = 1\ndx = ") + bad + "\ndy = 0\n");
        EXPECT_TRUE(names(dx, "line 4", "-32..32")) << "dx " << bad;
        const ScenarioParse dy = load_scenario_data(
            std::string("[[group]]\nkind = \"tank\"\ncount = 1\ndx = 0\ndy = ") + bad + "\n");
        EXPECT_TRUE(names(dy, "line 5", "-32..32")) << "dy " << bad;
    }
    EXPECT_TRUE(
        load_scenario_data("[[group]]\nkind = \"tank\"\ncount = 1\ndx = 32\ndy = -32\n").ok);
}

TEST(ScenarioToml, MoreThanSixtyFourUnitsASideNamesTheLine) {
    const std::string group = "[[group]]\nkind = \"tank\"\ncount = 32\ndx = 0\ndy = 0\n";
    EXPECT_TRUE(load_scenario_data(group + group).ok) << "64 is the bound, and it is allowed";
    // The third group (lines 11-15) is the one that takes the side past 64; the line named is its.
    const ScenarioParse parse = load_scenario_data(
        group + group + "[[group]]\nkind = \"turret\"\ncount = 1\ndx = 0\ndy = 0\n");
    EXPECT_TRUE(names(parse, "line 11", "64"));
}

TEST(ScenarioToml, ChangesTheSpawnComposition) {
    const ScenarioParse parse = load_scenario_data(R"([[group]]
kind = "factory"
count = 1
dx = 0
dy = 0

[[group]]
kind = "mortar_bike"
count = 1
dx = -4
dy = -1

[[group]]
kind = "rocket_tank"
count = 1
dx = -1
dy = -4

[[group]]
kind = "turret"
count = 3
dx = 5
dy = -3
)");
    ASSERT_TRUE(parse.ok) << parse.error;
    const Scenario s = skirmish(42, parse.data);
    EXPECT_EQ(s.seed, 42U);
    const std::vector<UnitKind> want{UnitKind::Factory, UnitKind::MortarBike, UnitKind::RocketTank,
                                     UnitKind::Turret,  UnitKind::Turret,     UnitKind::Turret};
    ASSERT_EQ(s.spawns.size(), 2 * want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(s.spawns.at(i).side, Side::Player) << i;
        EXPECT_EQ(s.spawns.at(i).kind, want.at(i)) << i;
        EXPECT_EQ(s.spawns.at(want.size() + i).side, Side::Enemy) << i;
        EXPECT_EQ(s.spawns.at(want.size() + i).kind, want.at(i)) << i;
    }
    // The factory stands on each base centre, so the mortar bike and the rocket tank show the
    // offsets: the player's lie at (-4,-1) and (-1,-4) of its factory, the enemy's mirror them.
    const Spawn& pf = s.spawns.at(0);
    const Spawn& ef = s.spawns.at(want.size());
    EXPECT_EQ(s.spawns.at(1).tile_x, pf.tile_x - 4);
    EXPECT_EQ(s.spawns.at(1).tile_y, pf.tile_y - 1);
    EXPECT_EQ(s.spawns.at(2).tile_x, pf.tile_x - 1);
    EXPECT_EQ(s.spawns.at(2).tile_y, pf.tile_y - 4);
    EXPECT_EQ(s.spawns.at(want.size() + 1).tile_x, ef.tile_x + 4);
    EXPECT_EQ(s.spawns.at(want.size() + 1).tile_y, ef.tile_y + 1);
    EXPECT_EQ(s.spawns.at(want.size() + 2).tile_x, ef.tile_x + 1);
    EXPECT_EQ(s.spawns.at(want.size() + 2).tile_y, ef.tile_y + 4);
    // Three turrets share one wish, so the placement rule spreads them over distinct tiles.
    for (std::size_t i = 0; i < s.spawns.size(); ++i) {
        for (std::size_t j = i + 1; j < s.spawns.size(); ++j) {
            EXPECT_FALSE(s.spawns.at(i).tile_x == s.spawns.at(j).tile_x
                         && s.spawns.at(i).tile_y == s.spawns.at(j).tile_y)
                << "spawns " << i << " and " << j << " share a tile";
        }
    }
}

/// Mutated and truncated scenarios are survived, and the answer is always one of the two: refused
/// with a reason, or accepted within every bound. (The libFuzzer target for rule 5 is a separate
/// slice: the replay entry already owns LLVMFuzzerTestOneInput in this binary.)
TEST(ScenarioToml, HostileGeneratedInputIsSurvived) {
    std::uint64_t state = 0x5CE7A210ULL;
    const auto next = [&state] {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    const std::string alphabet = "[]=#\"\n\r\t -+.0123456789abcdgknorstuxyz_";
    const std::string seed_text = kGroups;
    for (int round = 0; round < 4000; ++round) {
        std::string text = seed_text.substr(0, next() % (seed_text.size() + 1));
        for (std::uint64_t edits = next() % 6; edits > 0 && !text.empty(); --edits) {
            const std::size_t at = next() % text.size();
            const char c = alphabet[next() % alphabet.size()];
            if (next() % 2 == 0) {
                text[at] = c;
            } else {
                text.insert(text.begin() + static_cast<std::ptrdiff_t>(at), c);
            }
        }
        const ScenarioParse parse = load_scenario_data(text);
        if (!parse.ok) {
            EXPECT_FALSE(parse.error.empty()) << "refused without a reason: " << text;
            continue;
        }
        int units = 0;
        ASSERT_FALSE(parse.data.groups.empty()) << text;
        for (const Group& group : parse.data.groups) {
            EXPECT_GE(group.count, 1);
            EXPECT_LE(group.count, kMaxGroupCount);
            EXPECT_LE(std::abs(group.dx), kMaxOffset);
            EXPECT_LE(std::abs(group.dy), kMaxOffset);
            units += group.count;
        }
        EXPECT_LE(units, kMaxSideUnits) << text;
    }
}

// --- the file -----------------------------------------------------------------------------------

TEST(ScenarioToml, AFileThatCannotBeReadIsAnError) {
    const ScenarioParse parse = load_scenario_file("/nonexistent/dir/scenario.toml");
    EXPECT_FALSE(parse.ok);
    EXPECT_NE(parse.error.find("/nonexistent/dir/scenario.toml"), std::string::npos) << parse.error;
}

TEST(ScenarioToml, AFileIsReadThroughTheSameParser) {
    const std::string path = ::testing::TempDir() + "scenario_toml_test.toml";
    {
        std::ofstream file(path);
        file << kGroups;
    }
    const ScenarioParse good = load_scenario_file(path);
    ASSERT_TRUE(good.ok) << good.error;
    EXPECT_EQ(good.data.groups.size(), 2U);
    {
        std::ofstream file(path);
        file << "[[group]]\nkind = \"tnak\"\n";
    }
    EXPECT_TRUE(names(load_scenario_file(path), "line 2", "tnak"));
}

} // namespace
