// The replay format is both a file format and the fuzzer's input language, so it gets two bars:
// a malformed line must come back as an ERROR, and the canonical form must be exactly stable.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "protekt/sim/replay.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using namespace protekt::sim;

/// Parses or fails the test, so the result can be used without a check clang-tidy cannot see.
Replay must_parse(std::string_view text) {
    const auto parsed = parse(text);
    if (!parsed.has_value()) {
        ADD_FAILURE() << "expected this to parse: " << text;
        return Replay{};
    }
    return *parsed;
}

std::string parse_error(std::string_view text) {
    std::string error;
    const auto r = parse(text, &error);
    EXPECT_FALSE(r.has_value()) << "expected a rejection: " << text;
    EXPECT_FALSE(error.empty()) << "a rejection must say why";
    return error;
}

Replay sample_replay() {
    Replay r;
    r.seed = 42;
    // Aggregate assignment rather than three push_backs: GCC 14 at -O2 emits a bogus
    // -Wstringop-overflow for a push_back of an aggregate here, and the gate builds Release with
    // -Werror. Fix the code, never the flag.
    r.commands = {
        Command{0, Order{OrderKind::GuardAt,
                         UnitId{5},
                         Fixed::from_int(24),
                         Fixed::from_int(230),
                         {},
                         {},
                         {}}},
        Command{30, Order{OrderKind::Attack, UnitId{1}, {}, {}, {}, {}, UnitId{9}}},
        Command{299, Order{OrderKind::Stop, UnitId{1}, {}, {}, {}, {}, {}}},
    };
    return r;
}

TEST(Replay, HeaderAndSeedAreWrittenExactly) {
    const std::string text = to_text(sample_replay());
    const std::vector<std::string> lines = [&text] {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : text) {
            if (c == '\n') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
        return out;
    }();
    ASSERT_GE(lines.size(), 5U);
    EXPECT_EQ(lines[0], "# protekt replay v1");
    EXPECT_EQ(lines[1], "seed 42");
    EXPECT_EQ(lines[2], "0 guard 5 1572864 15073280");
    EXPECT_EQ(lines[3], "30 attack 1 9");
    EXPECT_EQ(lines[4], "299 stop 1");
}

TEST(Replay, CanonicalFormIsStableByteForByte) {
    const Replay r = sample_replay();
    const std::string canonical = to_text(r);
    const Replay again = must_parse(canonical);
    EXPECT_EQ(to_text(again), canonical);
    EXPECT_EQ(again.seed, r.seed);
    ASSERT_EQ(again.commands.size(), r.commands.size());
    for (std::size_t i = 0; i < r.commands.size(); ++i) {
        EXPECT_EQ(again.commands[i].tick, r.commands[i].tick);
        EXPECT_EQ(again.commands[i].order.kind, r.commands[i].order.kind);
        EXPECT_EQ(again.commands[i].order.unit, r.commands[i].order.unit);
        EXPECT_EQ(again.commands[i].order.target, r.commands[i].order.target);
        EXPECT_EQ(again.commands[i].order.x, r.commands[i].order.x);
        EXPECT_EQ(again.commands[i].order.y, r.commands[i].order.y);
        EXPECT_EQ(again.commands[i].order.x2, r.commands[i].order.x2);
        EXPECT_EQ(again.commands[i].order.y2, r.commands[i].order.y2);
    }
}

TEST(Replay, AcceptsBothPositionSpellings) {
    const Replay raw = must_parse("# protekt replay v1\nseed 1\n0 move 1 3932160 1310720\n");
    EXPECT_EQ(raw.commands.at(0).order.x, Fixed::from_int(60));
    EXPECT_EQ(raw.commands.at(0).order.y, Fixed::from_raw(1310720));

    const Replay decimal = must_parse("# protekt replay v1\nseed 1\n0 move 1 24.5 20.25\n");
    EXPECT_EQ(decimal.commands.at(0).order.x.raw, 24 * 65536 + 32768);
    EXPECT_EQ(decimal.commands.at(0).order.y.raw, 20 * 65536 + 16384);

    const Replay negative = must_parse("# protekt replay v1\nseed 1\n0 move 1 -1.5 0\n");
    EXPECT_EQ(negative.commands.at(0).order.x.raw, -(1 * 65536 + 32768));
}

TEST(Replay, ToleratesCommentsBlanksAndCrlf) {
    const Replay r = must_parse("# protekt replay v1\r\n\r\nseed 42\r\n# a comment\r\n\r\n"
                                "0 stop 1\r\n\n");
    EXPECT_EQ(r.seed, 42U);
    ASSERT_EQ(r.commands.size(), 1U);
    EXPECT_EQ(r.commands.at(0).order.kind, OrderKind::Stop);
}

TEST(Replay, MalformedInputIsRejectedWithAReason) {
    parse_error("");
    parse_error("seed 42\n");
    parse_error("# protekt replay v2\nseed 42\n");
    parse_error("# protekt replay v1\n");
    parse_error("# protekt replay v1\n0 stop 1\nseed 42\n");
    parse_error("# protekt replay v1\nseed 42\nseed 43\n");
    parse_error("# protekt replay v1\nseed 42\n0 teleport 1 1 1\n");
    parse_error("# protekt replay v1\nseed 42\n0 move 1 1\n");
    parse_error("# protekt replay v1\nseed 42\n0 move 1 1 2 3\n");
    parse_error("# protekt replay v1\nseed 42\n0 stop\n");
    parse_error("# protekt replay v1\nseed 42\n0 move one 1 2\n");
    parse_error("# protekt replay v1\nseed 42\n0 move 1 1.2.3 2\n");
    parse_error("# protekt replay v1\nseed 42\n0 move 1 99999999999999999999 0\n");
    parse_error("# protekt replay v1\nseed 99999999999999999999999\n");
    parse_error("# protekt replay v1\nseed 42\n100 stop 1\n0 stop 1\n");
    parse_error("# protekt replay v1\nseed 42\n0 stop 1\n0 stop\n");
}

TEST(Replay, RejectsNegativeTicksAndIds) {
    parse_error("# protekt replay v1\nseed 42\n-1 stop 1\n");
    parse_error("# protekt replay v1\nseed 42\n0 stop 0\n");
}

TEST(Replay, RunStepsToTheTickAfterTheLastCommand) {
    const Replay r = sample_replay();
    // The same battle, built by hand: this pins run()'s tick semantics against an explicit reading
    // of the documentation rather than against whatever run() happens to do.
    Sim sim = Sim::create(skirmish(r.seed));
    std::size_t next = 0;
    const Tick last = r.commands.empty() ? 0 : r.commands.back().tick;
    const Tick steps = std::max<Tick>(1, last + 1);
    for (Tick t = 0; t < steps; ++t) {
        while (next < r.commands.size() && r.commands[next].tick == t) {
            sim.enqueue(r.commands[next].order);
            ++next;
        }
        sim.step();
    }
    EXPECT_EQ(sim.tick(), steps);
    EXPECT_EQ(run(r), sim.state_hash());
}

TEST(Replay, RunIsDeterministicAndSeedSensitive) {
    const Replay r = sample_replay();
    EXPECT_EQ(run(r), run(r));
    Replay other = r;
    other.seed = 43;
    EXPECT_NE(run(r), run(other));
    Replay empty = r;
    empty.commands.clear();
    EXPECT_NE(run(empty), 0U);
    EXPECT_EQ(run(empty), run(empty));
}

TEST(Replay, EmittedCommandsAreInNonDecreasingTickOrder) {
    Replay r;
    r.seed = 5;
    r.commands = {
        Command{90, Order{OrderKind::Stop, UnitId{1}, {}, {}, {}, {}, {}}},
        Command{10, Order{OrderKind::Stop, UnitId{2}, {}, {}, {}, {}, {}}},
    };
    const Replay parsed = must_parse(to_text(r));
    for (std::size_t i = 1; i < parsed.commands.size(); ++i) {
        EXPECT_LE(parsed.commands[i - 1].tick, parsed.commands[i].tick);
    }
}

} // namespace
