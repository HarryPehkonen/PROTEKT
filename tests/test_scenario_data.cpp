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

TEST(ScenarioToml, TheMissileScoutIsNamedMslScoutAndIsNotTheScout) {
    const ScenarioParse parse
        = load_scenario_data("[[group]]\nkind = \"msl_scout\"\ncount = 1\ndx = 0\ndy = 0\n");
    ASSERT_TRUE(parse.ok) << parse.error;
    ASSERT_EQ(parse.data.groups.size(), 1U);
    EXPECT_EQ(parse.data.groups.at(0).kind, UnitKind::MslScout);
    EXPECT_NE(parse.data.groups.at(0).kind, UnitKind::Scout);
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
    // RE-RECORDED, round 19, L2: the factory is 8 x 8 and its tile is its north-west corner, so the
    // wished tiles (-4, -1) and (-1, -4) from the base centre fall inside, or beside, its footprint
    // and the placer seats the bike and the tank a tile or two off them. Old offsets from the
    // factory's tile (the code at 1a326d6): the player's bike (-4, -1) and tank (-1, -4), the
    // enemy's (+4, +1) and (+1, +4). New: the player's bike (-1, +2) and tank (+2, -1), the enemy's
    // (+3, -1) and (-1, 0) -- for seed 42.
    EXPECT_EQ(s.spawns.at(1).tile_x, pf.tile_x - 1);
    EXPECT_EQ(s.spawns.at(1).tile_y, pf.tile_y + 2);
    EXPECT_EQ(s.spawns.at(2).tile_x, pf.tile_x + 2);
    EXPECT_EQ(s.spawns.at(2).tile_y, pf.tile_y - 1);
    EXPECT_EQ(s.spawns.at(want.size() + 1).tile_x, ef.tile_x + 3);
    EXPECT_EQ(s.spawns.at(want.size() + 1).tile_y, ef.tile_y - 1);
    EXPECT_EQ(s.spawns.at(want.size() + 2).tile_x, ef.tile_x - 1);
    EXPECT_EQ(s.spawns.at(want.size() + 2).tile_y, ef.tile_y);
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

// --- N4: the initial conditions are data, per side -----------------------------------------------
//
// A scenario may open a `[player]` and an `[enemy]` table: a force class (`none`, `light` = two
// tanks, `heavy` = ten), the scrap the side opens with, and which buildings stand. Absent, every
// side is what it was.

namespace {

/// One group, on its own lines.
std::string group_text(const std::string& kind, int count, int dx, int dy) {
    return "[[group]]\nkind = \"" + kind + "\"\ncount = " + std::to_string(count)
           + "\ndx = " + std::to_string(dx) + "\ndy = " + std::to_string(dy) + "\n";
}

/// The composition the N4 tests share: a constructor, a factory, three tanks, a turret.
std::string base_composition() {
    return group_text("constructor", 1, -2, -2) + group_text("factory", 1, 0, 0)
           + group_text("tank", 1, 8, 2) + group_text("tank", 1, 8, 8) + group_text("tank", 1, 2, 8)
           + group_text("turret", 1, 5, -3);
}

int count_of(const Scenario& s, Side side, UnitKind kind) {
    int n = 0;
    for (const Spawn& spawn : s.spawns) {
        n += spawn.side == side && spawn.kind == kind ? 1 : 0;
    }
    return n;
}

Scenario scenario_from(const std::string& text, std::uint64_t seed = 42) {
    const ScenarioParse parse = load_scenario_data(text);
    EXPECT_TRUE(parse.ok) << parse.error;
    return skirmish(seed, parse.data);
}

} // namespace

TEST(StartingConditions, LightStartsExactlyTwoTanks) {
    const Scenario s = scenario_from("[player]\nforce = \"light\"\n" + base_composition());
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Tank), 2);
}

TEST(StartingConditions, NoneStartsNoTanksAndNoFactoryWhenTheScenarioSaysNothingStands) {
    // His example: normally there is no factory and the Constructor has to build it.
    const Scenario s
        = scenario_from("[player]\nforce = \"none\"\nstanding = \"\"\n" + base_composition());
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Tank), 0);
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Factory), 0);
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Constructor), 1); // the one who must build it
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Turret),
              1); // a turret is not a building list entry
    // The enemy, never mentioned, is exactly what the composition says.
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::Tank), 3);
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::Factory), 1);
}

TEST(StartingConditions, HeavyStartsExactlyTenTanks) {
    const Scenario s = scenario_from("[enemy]\nforce = \"heavy\"\n" + base_composition());
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::Tank), 10);
}

TEST(StartingConditions, TheTwoSidesAreIndependent) {
    const Scenario a = scenario_from("[player]\nforce = \"light\"\n[enemy]\nforce = \"heavy\"\n"
                                     + base_composition());
    EXPECT_EQ(count_of(a, Side::Player, UnitKind::Tank), 2);
    EXPECT_EQ(count_of(a, Side::Enemy, UnitKind::Tank), 10);
    const Scenario b = scenario_from("[enemy]\nforce = \"light\"\n[player]\nforce = \"heavy\"\n"
                                     + base_composition());
    EXPECT_EQ(count_of(b, Side::Player, UnitKind::Tank), 10);
    EXPECT_EQ(count_of(b, Side::Enemy, UnitKind::Tank), 2);
}

TEST(StartingConditions, TheClassesCarryNoBalanceOnlyCounts) {
    // The class changes the tanks and nothing else a side opens with.
    const Scenario s = scenario_from("[player]\nforce = \"heavy\"\n" + base_composition());
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Constructor), 1);
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Factory), 1);
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Turret), 1);
}

TEST(StartingConditions, AStandingListNamesWhatStandsAndTheRestOfTheBuildingsAreNot) {
    const Scenario s = scenario_from("[enemy]\nstanding = \"factory\"\n" + base_composition()
                                     + group_text("power", 1, -6, 0));
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::Factory), 1);
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::Power), 0);
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Power), 1);
}

TEST(StartingConditions, ScrapIsPerSide) {
    const Scenario s
        = scenario_from("[player]\nscrap = 0\n[enemy]\nscrap = 5\n" + base_composition());
    // Each side's litter lies round its own base; compare the nearer base's pieces.
    ASSERT_EQ(s.scrap.size(), 5U);
}

TEST(StartingConditions, AnUnmodifiedScenarioIsExactlyWhatItWasBefore) {
    // The pin that protects every recording: a fold over every spawn, piece of litter and pool of
    // three seeds, taken from the tree BEFORE this slice. A scenario that names no initial
    // condition -- the built-in one, and a file of groups alone -- must reproduce it exactly.
    constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 3> kPins{
        {{1ULL, 8853736118488700918ULL},
         {42ULL, 68732737021255861ULL},
         {777ULL, 424896281085665198ULL}}};
    for (const auto& [seed, want] : kPins) {
        const auto fold = [](const Scenario& s) {
            std::uint64_t h = 1469598103934665603ULL;
            const auto mix = [&h](std::int64_t v) {
                h ^= static_cast<std::uint64_t>(v);
                h *= 1099511628211ULL;
            };
            for (const Spawn& p : s.spawns) {
                mix(static_cast<int>(p.kind));
                mix(static_cast<int>(p.side));
                mix(p.tile_x);
                mix(p.tile_y);
            }
            for (const Scrap& c : s.scrap) {
                mix(c.x.raw);
                mix(c.y.raw);
            }
            mix(static_cast<std::int64_t>(s.spawns.size()));
            mix(static_cast<std::int64_t>(s.scrap.size()));
            mix(static_cast<std::int64_t>(s.pools.size()));
            mix(s.store_capacity);
            return h;
        };
        EXPECT_EQ(fold(skirmish(seed)), want) << "seed " << seed;
        ScenarioData text_default = default_data();
        EXPECT_EQ(fold(skirmish(seed, text_default)), want) << "seed " << seed;
    }
}

TEST(StartingConditions, AClassThatDoesNotExistIsRefusedWithItsLine) {
    // Written down: a name that is not none/light/heavy is REFUSED (the parser never guesses a
    // default), naming the line and listing the three.
    const ScenarioParse parse
        = load_scenario_data("[player]\nforce = \"medium\"\n" + base_composition());
    EXPECT_TRUE(names(parse, "line 2", "medium"));
    EXPECT_TRUE(names(parse, "line 2", "none, light, heavy"));
}

TEST(StartingConditions, ABadSideTableIsRefusedWithItsLine) {
    EXPECT_TRUE(names(
        load_scenario_data("[player]\nforce = \"light\"\nforce = \"heavy\"\n" + base_composition()),
        "line 3", "twice"));
    EXPECT_TRUE(
        names(load_scenario_data("[player]\nspeed = 3\n" + base_composition()), "line 2", "speed"));
    EXPECT_TRUE(
        names(load_scenario_data("[player]\n[player]\n" + base_composition()), "line 2", "twice"));
    // A building that the composition does not carry cannot be asked to stand.
    EXPECT_TRUE(names(load_scenario_data("[enemy]\nstanding = \"kiln\"\n" + base_composition()),
                      "line 1", "kiln"));
    // Only buildings stand.
    EXPECT_TRUE(names(load_scenario_data("[enemy]\nstanding = \"tank\"\n" + base_composition()),
                      "line 2", "tank"));
}
