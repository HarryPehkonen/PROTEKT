// protekt-map is run as a built binary, because its whole point is a separate process. A refusal is
// only a refusal if the exit code is 2, stderr says what was wanted, and no world is printed.
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct MapRun {
    int code{};
    std::string out{};
    std::string err{};
};

std::string slurp(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Runs protekt-map with `args` (each one single-quoted for the shell) and captures both streams.
MapRun run_map(const std::vector<std::string>& args) {
    static int counter = 0;
    const fs::path stem
        = fs::path(::testing::TempDir())
          / ("protekt-map-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    const fs::path out_file = stem.string() + ".out";
    const fs::path err_file = stem.string() + ".err";
    std::string command = std::string{PROTEKT_MAP};
    for (const std::string& arg : args) {
        command += " '" + arg + "'";
    }
    command += " >" + out_file.string() + " 2>" + err_file.string();
    const int status = std::system(command.c_str());
    MapRun run{WIFEXITED(status) ? WEXITSTATUS(status) : -1, slurp(out_file), slurp(err_file)};
    std::error_code ignored;
    fs::remove(out_file, ignored);
    fs::remove(err_file, ignored);
    return run;
}

void expect_refused(const std::vector<std::string>& args, const std::string& message) {
    const MapRun r = run_map(args);
    EXPECT_EQ(r.code, 2) << "args: " << (args.empty() ? "" : args[0]) << "\nstderr: " << r.err;
    EXPECT_EQ(r.err, message);
    EXPECT_TRUE(r.out.empty()) << "a refused command line must not print a world:\n" << r.out;
}

TEST(MapCli, ANonNumericSeedIsRefusedNotReadAsOne) {
    expect_refused({"--seed", "abc"}, "protekt-map: --seed wants an unsigned integer, got 'abc'\n");
    expect_refused({"--seed", "42x"}, "protekt-map: --seed wants an unsigned integer, got '42x'\n");
}

TEST(MapCli, AnUnknownOptionIsRefused) {
    expect_refused({"--nonsense"}, "protekt-map: unknown option '--nonsense'\n");
    expect_refused({"--size", "4", "-x"}, "protekt-map: unknown option '-x'\n");
}

TEST(MapCli, EveryOptionMissingItsValueIsRefused) {
    expect_refused({"--seed"}, "protekt-map: --seed needs a value\n");
    expect_refused({"--size"}, "protekt-map: --size needs a value\n");
    expect_refused({"--chunk"}, "protekt-map: --chunk needs a value\n");
    expect_refused({"--chunk", "1"}, "protekt-map: --chunk needs a value\n");
}

TEST(MapCli, ABadSizeOrChunkIsRefused) {
    expect_refused({"--size", "abc"}, "protekt-map: --size wants an integer, got 'abc'\n");
    expect_refused({"--size", "99999999999"},
                   "protekt-map: --size wants an integer, got '99999999999'\n");
    expect_refused({"--chunk", "a", "b"}, "protekt-map: --chunk wants two integers, got 'a' 'b'\n");
    expect_refused({"--chunk", "1", "2.5"},
                   "protekt-map: --chunk wants two integers, got '1' '2.5'\n");
}

TEST(MapCli, AValidCommandLineStillPrintsAWorldAndExitsZero) {
    const MapRun r = run_map({"--seed", "42", "--size", "8"});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(r.err.empty()) << r.err;
    EXPECT_EQ(r.out.rfind("seed 0x000000000000002A  chunk (0,0)  8x8\n", 0), 0U) << r.out;
    EXPECT_NE(r.out.find("\nhash 0x"), std::string::npos) << r.out;
    int rows = 0;
    std::istringstream lines{r.out};
    for (std::string line; std::getline(lines, line);) {
        rows += line.size() == 8 && line.find_first_not_of("~-.+^") == std::string::npos;
    }
    EXPECT_EQ(rows, 8);
}

TEST(MapCli, TheDocumentedClampOfSizeIsStillThere) {
    // The 2..64 clamp is policy, not a refusal: a number that parses is clamped, as documented.
    EXPECT_EQ(run_map({"--size", "1"}).out.rfind("seed 0x0000000000000001  chunk (0,0)  2x2\n", 0),
              0U);
    EXPECT_EQ(
        run_map({"--size", "9999"}).out.rfind("seed 0x0000000000000001  chunk (0,0)  64x64\n", 0),
        0U);
}

TEST(MapCli, AChunkAtTheEdgeOfIntIsPrintedNotUndefined) {
    // chunk+1 is the region the hash covers; at INT_MAX it must not overflow. A negative
    // other coordinate keeps the hashed region empty, so the run stays instant.
    const MapRun hi = run_map({"--seed", "42", "--size", "8", "--chunk", "2147483647", "-1"});
    EXPECT_EQ(hi.code, 0) << hi.err;
    EXPECT_EQ(hi.out.rfind("seed 0x000000000000002A  chunk (2147483647,-1)  8x8\n", 0), 0U)
        << hi.out;
    const MapRun lo = run_map({"--seed", "42", "--size", "8", "--chunk", "-2147483648", "0"});
    EXPECT_EQ(lo.code, 0) << lo.err;
    EXPECT_EQ(lo.out.rfind("seed 0x000000000000002A  chunk (-2147483648,0)  8x8\n", 0), 0U)
        << lo.out;
}

} // namespace
