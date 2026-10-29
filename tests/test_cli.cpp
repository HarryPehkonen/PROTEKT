// The CLI is a function, not a main(), so its output can be asserted line for line. The money test
// here is that a battle recorded to a file replays to the byte-identical log: that is what proves
// the format is lossless rather than merely parseable.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "protekt/app/cli.hpp"

namespace {

using namespace protekt::app;

struct CliResult {
    int code{};
    std::string out{};
    std::string err{};
};

CliResult run(const std::vector<std::string>& args) {
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const std::string& a : args) {
        views.emplace_back(a);
    }
    std::ostringstream out;
    std::ostringstream err;
    const int code = run_cli(views, out, err);
    return CliResult{code, out.str(), err.str()};
}

std::filesystem::path temp_file(const std::string& name) {
    return std::filesystem::path(::testing::TempDir()) / name;
}

std::string end_line_of(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            last = line;
        }
    }
    return last;
}

TEST(Cli, DefaultRunPrintsABattleAndExitsZero) {
    const CliResult r = run({"--seed", "42", "--ticks", "120"});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(r.err.empty());
    EXPECT_NE(r.out.find("status unit"), std::string::npos)
        << "the log should show the battlefield";
    EXPECT_NE(r.out.find("end tick 120 seed 42"), std::string::npos);
}

TEST(Cli, RecordingAndReplayingGivesTheByteIdenticalLog) {
    const std::filesystem::path file = temp_file("protekt-cli-roundtrip.replay");
    const std::vector<std::string> live{"--seed", "42",           "--ticks",
                                        "400",    "--replay-out", file.string()};
    const CliResult first = run(live);
    ASSERT_EQ(first.code, 0) << first.err;
    ASSERT_TRUE(std::filesystem::exists(file));

    const CliResult second = run({"--replay", file.string(), "--ticks", "400"});
    ASSERT_EQ(second.code, 0) << second.err;
    EXPECT_EQ(second.out, first.out) << "a replay must reproduce the battle it recorded";
    EXPECT_EQ(end_line_of(second.out), end_line_of(first.out));
}

TEST(Cli, WithoutACommanderNothingIsOrderedAndNothingIsFired) {
    const CliResult r = run({"--seed", "42", "--ticks", "200", "--commander", "none"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out.find("fired "), std::string::npos)
        << "neither side was commanded and they start 100 tiles apart";
    EXPECT_EQ(r.out.find("ordered "), std::string::npos);
    EXPECT_NE(r.out.find("end tick 200"), std::string::npos);
}

TEST(Cli, StatusPeriodZeroPrintsEventsOnly) {
    const CliResult r = run({"--seed", "42", "--ticks", "300", "--status-period", "0"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out.find("status unit"), std::string::npos);
    EXPECT_NE(r.out.find("end tick 300"), std::string::npos);
}

TEST(Cli, TheSeedChangesTheBattle) {
    const CliResult a = run({"--seed", "42", "--ticks", "300"});
    const CliResult b = run({"--seed", "43", "--ticks", "300"});
    ASSERT_EQ(a.code, 0);
    ASSERT_EQ(b.code, 0);
    EXPECT_NE(end_line_of(a.out), end_line_of(b.out)) << "the summary carries the state hash";
}

/// How many orders a unit id was given, read from the log's own "ordered unit <id>" lines.
int orders_for(const std::string& text, int id) {
    const std::string needle = " ordered unit " + std::to_string(id) + " ";
    int count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

TEST(Cli, TheUtilityCommanderFightsFromTheCommandLine) {
    // The AI this project measures has to be the AI a person can WATCH: before this, the only way
    // to see the utility commander fight was to read a test's output, and a commander nobody can
    // run is a commander nobody can judge. The enemy is the utility one; the player stays scripted,
    // so this is a real battle rather than two identical armies.
    const CliResult r = run(
        {"--seed", "42", "--ticks", "1800", "--commander", "utility", "--status-period", "0"});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_NE(r.out.find(" killed unit "), std::string::npos)
        << "the fight has to resolve, not just start";

    // Ids 1-6 are the player's and 7-12 the enemy's, from the frozen skirmish composition (3 tanks,
    // 1 factory, 2 turrets a side, ids handed out in spawn order).
    EXPECT_GT(orders_for(r.out, 7) + orders_for(r.out, 8) + orders_for(r.out, 9), 0)
        << "the utility commander issued nothing at all";
    EXPECT_GT(orders_for(r.out, 1) + orders_for(r.out, 2) + orders_for(r.out, 3), 0)
        << "and the other side issued nothing either";
}

TEST(Cli, TheUtilityCommanderRunIsReproducible) {
    const CliResult first = run({"--seed", "42", "--ticks", "1200", "--commander", "utility"});
    const CliResult second = run({"--seed", "42", "--ticks", "1200", "--commander", "utility"});
    EXPECT_EQ(first.code, 0);
    EXPECT_EQ(first.out, second.out)
        << "the same seed and the same commanders must give the same log, byte for byte";
}

TEST(Cli, UsageErrorsAreReportedAndExitTwo) {
    for (const std::vector<std::string>& bad :
         std::vector<std::vector<std::string>>{{"--nonsense"},
                                               {"--seed"},
                                               {"--seed", "abc"},
                                               {"--commander", "bogus"},
                                               {"--ticks", "-5"},
                                               {"extra-positional"}}) {
        const CliResult r = run(bad);
        EXPECT_EQ(r.code, 2) << "should be a usage error: " << (bad.empty() ? "" : bad[0]);
        EXPECT_FALSE(r.err.empty()) << "a usage error must say something";
        EXPECT_TRUE(r.out.empty()) << "no battle should be printed";
    }
}

TEST(Cli, ABrokenReplayIsAnErrorNotACrash) {
    const std::filesystem::path file = temp_file("protekt-cli-broken.replay");
    {
        std::ofstream out(file);
        out << "this is not a replay at all\n";
    }
    const CliResult r = run({"--replay", file.string()});
    EXPECT_EQ(r.code, 3);
    EXPECT_FALSE(r.err.empty());

    const CliResult missing = run({"--replay", temp_file("definitely-not-here.replay").string()});
    EXPECT_EQ(missing.code, 3);
    EXPECT_FALSE(missing.err.empty());
}

// --- docs/CLI.md must say what the parsers accept ------------------------------------------------
//
// The flags live only in the parsers, so the reference is a second copy that can rot. These tests
// read both from disk (the same rig as test_layering.cpp: PROTEKT_SOURCE_DIR and a file slurped)
// and fail in BOTH directions: a flag a parser accepts that the document does not list, and a flag
// the document names that no parser accepts.

namespace fs = std::filesystem;

std::string slurp_file(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Every flag a parser source accepts: a string literal that is exactly `"--name"`. Messages that
/// merely mention a flag inside a longer string are not matched, because they are not comparisons.
std::set<std::string> accepted_flags(const fs::path& source) {
    const std::string text = slurp_file(source);
    static const std::regex literal{R"re("(--[a-z][a-z0-9-]*)")re"};
    std::set<std::string> flags;
    for (std::sregex_iterator it{text.begin(), text.end(), literal}, end; it != end; ++it) {
        flags.insert((*it)[1].str());
    }
    return flags;
}

/// The flags in the first column of the reference tables of one `## <binary>` section: the lines
/// that begin "| `--flag`".
std::set<std::string> documented_flags(const std::string& doc, const std::string& binary) {
    std::set<std::string> flags;
    std::istringstream lines{doc};
    std::string line;
    bool inside = false;
    static const std::regex row{R"re(^\| `(--[a-z][a-z0-9-]*)`)re"};
    while (std::getline(lines, line)) {
        if (line.starts_with("## ")) {
            inside = line == "## " + binary;
        } else if (std::smatch m; inside && std::regex_search(line, m, row)) {
            flags.insert(m[1].str());
        }
    }
    return flags;
}

const std::map<std::string, std::string>& parser_sources() {
    static const std::map<std::string, std::string> sources{
        {"protekt-game", "src/app/game.cpp"},         {"protekt-render", "src/app/render.cpp"},
        {"protekt-sim", "src/app/cli.cpp"},           {"protekt-map", "src/app/map_cli.cpp"},
        {"protekt-atlas-gen", "tools/atlas_gen.cpp"},
    };
    return sources;
}

TEST(CliDoc, EveryFlagAParserAcceptsIsInTheDocumentAndNothingElseIs) {
    const fs::path root{PROTEKT_SOURCE_DIR};
    const std::string doc = slurp_file(root / "docs/CLI.md");
    ASSERT_FALSE(doc.empty()) << "docs/CLI.md is missing or empty";

    std::set<std::string> everything;
    for (const auto& [binary, source] : parser_sources()) {
        const std::set<std::string> accepted = accepted_flags(root / source);
        ASSERT_FALSE(accepted.empty()) << "found no flags in " << source << ": the scan is broken";
        everything.insert(accepted.begin(), accepted.end());

        const std::set<std::string> documented = documented_flags(doc, binary);
        for (const std::string& flag : accepted) {
            EXPECT_TRUE(documented.contains(flag))
                << binary << " accepts " << flag << " (" << source
                << ") but docs/CLI.md has no row for it under '## " << binary << "'";
        }
        for (const std::string& flag : documented) {
            EXPECT_TRUE(accepted.contains(flag))
                << "docs/CLI.md lists " << flag << " under '## " << binary
                << "' but that parser does not accept it";
        }
    }

    // Anywhere else in the document, a flag may be mentioned only if some parser really has it.
    static const std::regex mention{R"re(--[a-z][a-z0-9-]*)re"};
    for (std::sregex_iterator it{doc.begin(), doc.end(), mention}, end; it != end; ++it) {
        EXPECT_TRUE(everything.contains(it->str()))
            << "docs/CLI.md names " << it->str() << ", which no parser accepts";
    }
}

} // namespace
