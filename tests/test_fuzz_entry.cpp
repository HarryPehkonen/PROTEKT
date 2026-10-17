// A fuzz target nothing runs is decoration, so the suite drives the SAME entry point libFuzzer
// uses: the checked-in corpus on every gate run, plus generated hostile input. The corpus is the
// seed corpus from the replay documentation; the invalid entries are there on purpose, because a
// corpus that cannot fail proves nothing.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "protekt/sim/replay.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> corpus_files() {
    std::vector<std::filesystem::path> out;
    for (const auto& entry : std::filesystem::directory_iterator(PROTEKT_CORPUS_DIR)) {
        if (entry.is_regular_file()) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int feed(const std::string& text) {
    return LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

TEST(FuzzEntry, ThereIsACorpus) {
    EXPECT_GE(corpus_files().size(), 3U) << "the documented examples are the seed corpus";
}

TEST(FuzzEntry, TheValidCorpusEntryParsesAndTheInvalidOnesDoNot) {
    // Without this, a corpus of nothing-but-rejects would pass the survival test while testing
    // nothing about the accepting path.
    const std::string valid
        = slurp(std::filesystem::path(PROTEKT_CORPUS_DIR) / "valid_skirmish.replay");
    ASSERT_FALSE(valid.empty());
    std::string error;
    EXPECT_TRUE(protekt::sim::parse(valid, &error).has_value()) << error;

    for (const char* name :
         {"invalid_overflow.replay", "invalid_verb.replay", "invalid_order.replay"}) {
        const std::string text = slurp(std::filesystem::path(PROTEKT_CORPUS_DIR) / name);
        ASSERT_FALSE(text.empty()) << name;
        EXPECT_FALSE(protekt::sim::parse(text, &error).has_value())
            << name << " should be rejected";
        EXPECT_FALSE(error.empty());
    }
}

TEST(FuzzEntry, EveryCorpusFileIsSurvived) {
    for (const std::filesystem::path& path : corpus_files()) {
        const std::string text = slurp(path);
        EXPECT_EQ(feed(text), 0) << "corpus file: " << path.filename().string();
    }
}

TEST(FuzzEntry, HostileGeneratedInputIsSurvived) {
    // Deterministic pseudo-random bytes drawn from the format's own alphabet: mostly structured
    // enough to reach the parser's accepting path, with junk mixed in. A crash here is a bug in
    // parse(), and the artifact would become a regression test.
    const std::string alphabet = "0123456789.- \n#s e d m o v g u a r p t k c i l n y\\x00\\xff";
    std::uint64_t state = 0x5EEDULL;
    for (int i = 0; i < 3000; ++i) {
        const std::size_t len = static_cast<std::size_t>(splitmix64(state) % 120);
        std::string text;
        text.reserve(len);
        for (std::size_t j = 0; j < len; ++j) {
            text.push_back(alphabet[splitmix64(state) % alphabet.size()]);
        }
        const int rc = feed(text);
        ASSERT_EQ(rc, 0) << "input survived badly: " << text;
    }
}

TEST(FuzzEntry, TruncationsOfAValidReplayAreSurvived) {
    const std::string valid
        = slurp(std::filesystem::path(PROTEKT_CORPUS_DIR) / "valid_skirmish.replay");
    for (std::size_t n = 0; n <= valid.size(); ++n) {
        EXPECT_EQ(feed(valid.substr(0, n)), 0) << "truncation at " << n;
    }
}

} // namespace
