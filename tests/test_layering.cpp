// The sim sits at the bottom of the dependency graph: world <- sim <- {ai, cmd, input} <- game <-
// app. A rule nobody can check is a rule that decays, and this one already decayed once:
// `sim/observer.hpp` included `ai/commander.hpp` to name a parameter type, which is how the
// commander INTERFACE came to be declared in the AI's namespace while the sim was what implemented
// it. These tests read the sources on disk and fail if a lower layer reaches upward again, or if
// the tick loop reaches for the float maths.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// The layers built ON the sim, not into it. `input` is here and not below because it produces
/// orders
/// -- it is a client of the sim the same way the game is.
/// `audio` is here because the rule it enforces is the one that matters: the sim never learns that
/// sound exists.
constexpr std::array kUpperLayers{"ai", "audio", "cmd", "game", "input", "render", "app"};

/// The bottom, where a tick's worth of state lives and nothing from above may leak in.
constexpr std::array kLowerLayers{"sim", "world"};

[[nodiscard]] std::string slurp(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// Every #include line under the lower layers that names one of `forbidden`, with the file it is
/// in. Empty means clean; the lines are the report. Built as text rather than a vector of strings
/// because the caller only ever asks whether there are any, and prints them when there are.
[[nodiscard]] std::string
scan_lower_layers(const fs::path& root, const std::vector<std::string>& forbidden, int& scanned) {
    std::string violations;
    for (const char* lower : kLowerLayers) {
        for (const fs::path& dir : {root / "include/protekt" / lower, root / "src" / lower}) {
            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                ++scanned;
                std::istringstream text(slurp(entry.path()));
                std::string line;
                while (std::getline(text, line)) {
                    if (!line.starts_with("#include \"protekt/")) {
                        continue;
                    }
                    for (const std::string& bad : forbidden) {
                        if (line.find(bad) != std::string::npos) {
                            violations += entry.path().filename().string() + ": " + line + "\n";
                        }
                    }
                }
            }
        }
    }
    return violations;
}

} // namespace

TEST(Layering, NothingUnderSimOrWorldIncludesALayerAboveIt) {
    std::vector<std::string> forbidden(kUpperLayers.size());
    std::transform(kUpperLayers.begin(), kUpperLayers.end(), forbidden.begin(),
                   [](const char* layer) { return std::string{"protekt/"} + layer + "/"; });
    int scanned = 0;
    const std::string violations
        = scan_lower_layers(fs::path{PROTEKT_SOURCE_DIR}, forbidden, scanned);
    EXPECT_GT(scanned, 10)
        << "the scan found almost no files: it is not reading the tree it means to";
    EXPECT_TRUE(violations.empty())
        << "a lower layer reached upward. The sim defines the contracts it needs (ICommander, "
           "Observation, observe) and the layers above implement them -- if a sim file needs "
           "something "
           "from above, that thing belongs in sim/, not this include:\n"
        << violations;
}

/// The build enforces the same rule from the other side: `app/` lives in `protekt_app`, which alone
/// links the renderer and sees the window. The game's own sources must not name the renderer or
/// the app, or the link line stops being the thing that says so. (The game HEADERS still include
/// `render/renderer.hpp` for its plain-data Scene types; that is a known leak, not scanned here.)
TEST(Layering, NothingUnderSrcGameIncludesTheRendererOrTheApp) {
    int scanned = 0;
    std::string violations;
    for (const auto& entry :
         fs::recursive_directory_iterator(fs::path{PROTEKT_SOURCE_DIR} / "src/game")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        ++scanned;
        std::istringstream text(slurp(entry.path()));
        std::string line;
        while (std::getline(text, line)) {
            if (line.starts_with("#include \"protekt/render/")
                || line.starts_with("#include \"protekt/app/")) {
                violations += entry.path().filename().string() + ": " + line + "\n";
            }
        }
    }
    EXPECT_GT(scanned, 5) << "the scan found almost no files: it is not reading src/game";
    EXPECT_TRUE(violations.empty()) << "the game reached up into render/ or app/:\n" << violations;
}

/// Coding standard 13: the tick loop is Q16.16 integer, and `protekt/math/math.hpp` is the one
/// module allowed to hold a float. A float in the sim would make the world depend on the compiler,
/// and a replay would stop being bit-identical from one machine to the next.
TEST(Layering, NothingUnderSimOrWorldIncludesTheCameraMaths) {
    int scanned = 0;
    const std::string violations
        = scan_lower_layers(fs::path{PROTEKT_SOURCE_DIR}, {"protekt/math/"}, scanned);
    EXPECT_GT(scanned, 10);
    EXPECT_TRUE(violations.empty()) << "the simulation reached for the float maths:\n"
                                    << violations;
}

/// The scans are only as good as the lists they scan for, so the lists are pinned: these are the
/// project's layers as they exist, and a new one has to be named here deliberately.
TEST(Layering, TheLayerListsNameEveryLayerTheProjectHas) {
    std::vector<std::string> found;
    for (const auto& entry :
         fs::directory_iterator(fs::path{PROTEKT_SOURCE_DIR} / "include/protekt")) {
        if (entry.is_directory()) {
            found.push_back(entry.path().filename().string());
        }
    }
    ASSERT_GT(found.size(), 6U)
        << "the layer scan found no directories: it is looking in the wrong place";
    for (const std::string& layer : found) {
        const bool is_lower = std::any_of(std::begin(kLowerLayers), std::end(kLowerLayers),
                                          [&](const char* l) { return layer == l; });
        const bool is_upper = std::any_of(std::begin(kUpperLayers), std::end(kUpperLayers),
                                          [&](const char* l) { return layer == l; });
        EXPECT_TRUE(is_lower || is_upper || layer == "math")
            << "layer '" << layer
            << "' is neither a named lower layer, nor an upper one, nor the one float module: add "
               "it to "
               "the layering rule on purpose rather than by omission";
    }
}
