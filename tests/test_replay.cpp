// The replay format is both a file format and the fuzzer's input language, so it gets two bars:
// a malformed line must come back as an ERROR, and the canonical form must be exactly stable.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
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

/// The move a one-command replay holds.
const orders::MoveTo& move_of(const Replay& r) {
    return std::get<orders::MoveTo>(r.commands.at(0).order.payload);
}

/// The same order: same unit, same kind, and every field that kind carries. Fails if either side
/// holds a different alternative.
void expect_same_order(const Order& got, const Order& want) {
    EXPECT_EQ(got.unit, want.unit);
    ASSERT_EQ(got.kind(), want.kind());
    std::visit(
        [&got](const auto& w) {
            using P = std::decay_t<decltype(w)>;
            const P& g = std::get<P>(got.payload);
            if constexpr (std::is_same_v<P, orders::MoveTo> || std::is_same_v<P, orders::GuardAt>) {
                EXPECT_EQ(g.x, w.x);
                EXPECT_EQ(g.y, w.y);
            } else if constexpr (std::is_same_v<P, orders::Patrol>) {
                EXPECT_EQ(g.x, w.x);
                EXPECT_EQ(g.y, w.y);
                EXPECT_EQ(g.x2, w.x2);
                EXPECT_EQ(g.y2, w.y2);
            } else if constexpr (std::is_same_v<P, orders::FireTowards>
                                 || std::is_same_v<P, orders::FireLob>) {
                EXPECT_EQ(g.dir_x, w.dir_x);
                EXPECT_EQ(g.dir_y, w.dir_y);
                EXPECT_EQ(g.dir_z, w.dir_z)
                    << "a replay that drops the pitch drops every high shot";
                EXPECT_EQ(g.aimed_at, w.aimed_at)
                    << "a replay that drops the intent drops friendly fire";
            } else if constexpr (std::is_same_v<P, orders::Attack>
                                 || std::is_same_v<P, orders::FocusFire>) {
                EXPECT_EQ(g.target, w.target);
            } else if constexpr (std::is_same_v<P, orders::Deploy>) {
                EXPECT_EQ(g.tile_x, w.tile_x);
                EXPECT_EQ(g.tile_y, w.tile_y);
            }
        },
        want.payload);
}

Replay sample_replay() {
    Replay r;
    r.seed = 42;
    // Aggregate assignment rather than three push_backs: GCC 14 at -O2 emits a bogus
    // -Wstringop-overflow for a push_back of an aggregate here, and the gate builds Release with
    // -Werror. Fix the code, never the flag.
    r.commands = {
        Command{0, Order{UnitId{5}, orders::GuardAt{Fixed::from_int(24), Fixed::from_int(230)}}},
        Command{30, Order{UnitId{1}, orders::Attack{UnitId{9}}}},
        Command{299, Order{UnitId{1}, orders::Stop{}}},
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
        expect_same_order(again.commands[i].order, r.commands[i].order);
    }
}

TEST(Replay, EveryOrderKindSurvivesARoundTripAndNoneIsDropped) {
    // One order of each kind, built from the variant: a kind added to OrderPayload but not given a
    // spelling shows up here as a line missing from the text, not as a shot that silently never
    // fires.
    Replay r;
    r.seed = 7;
    r.commands = {
        Command{1, Order{UnitId{2}, orders::MoveTo{Fixed::from_int(10), Fixed::from_raw(-98304)}}},
        Command{2, Order{UnitId{3}, orders::GuardAt{Fixed::from_int(20), Fixed::from_int(30)}}},
        Command{3, Order{UnitId{4}, orders::Patrol{Fixed::from_int(1), Fixed::from_int(2),
                                                   Fixed::from_int(3), Fixed::from_raw(65537)}}},
        Command{4, Order{UnitId{5}, orders::Attack{UnitId{9}}}},
        Command{5, Order{UnitId{6}, orders::Stop{}}},
        Command{6, Order{UnitId{7}, orders::FocusFire{UnitId{11}}}},
        Command{
            7, Order{UnitId{8}, orders::FireTowards{Fixed::from_raw(-65536), Fixed::from_raw(46341),
                                                    UnitId{9}, Fixed::from_raw(-23170)}}},
        Command{8, Order{UnitId{2}, orders::Strafe{Fixed::from_raw(65536), Fixed::from_raw(-1)}}},
        Command{9, Order{UnitId{3}, orders::Follow{UnitId{4}}}},
        Command{10, Order{UnitId{5}, orders::FireLob{Fixed::from_raw(65536), Fixed::zero(),
                                                     UnitId{6}, Fixed::from_raw(131072)}}},
        Command{11, Order{UnitId{6}, orders::Deploy{12, 255}}},
        Command{12, Order{UnitId{7}, orders::Recycle{UnitId{8}}}},
        Command{13, Order{UnitId{8}, orders::FireFan{Fixed::from_raw(65536), Fixed::zero(),
                                                     UnitId{9}, Fixed::from_raw(-65536)}}},
        Command{14,
                Order{UnitId{9}, orders::FireRocket{Fixed::from_raw(65536), Fixed::from_raw(32768),
                                                    UnitId{10}, Fixed::from_raw(-65536)}}},
    };
    ASSERT_EQ(r.commands.size(), kOrderKindCount) << "this test must cover every kind";
    for (std::size_t i = 0; i < r.commands.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(r.commands.at(i).order.kind()), i)
            << "the commands above must be one per kind, in kind order";
    }

    const std::string text = to_text(r);
    EXPECT_EQ(static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')),
              2 + r.commands.size())
        << "header + seed + one line per order: a kind with no spelling is a dropped command";
    EXPECT_NE(text.find("6 focusfire 7 11\n"), std::string::npos) << text;
    EXPECT_NE(text.find("7 firetowards 8 -65536 46341 -23170 9\n"), std::string::npos) << text;
    EXPECT_NE(text.find("9 follow 3 4\n"), std::string::npos) << text;
    EXPECT_NE(text.find("11 deploy 6 786432 16711680\n"), std::string::npos)
        << "the tile is written as a whole-tile position: " << text;
    EXPECT_NE(text.find("12 recycle 7 8\n"), std::string::npos)
        << "a recycle names its recycler as the target: " << text;
    EXPECT_NE(text.find("13 firefan 8 65536 0 -65536 9\n"), std::string::npos) << text;
    EXPECT_NE(text.find("14 firerocket 9 65536 32768 -65536 10\n"), std::string::npos)
        << "the rocket records the aim, not its balance numbers: " << text;

    const Replay again = must_parse(text);
    ASSERT_EQ(again.commands.size(), r.commands.size());
    for (std::size_t i = 0; i < r.commands.size(); ++i) {
        EXPECT_EQ(again.commands.at(i).tick, r.commands.at(i).tick);
        expect_same_order(again.commands.at(i).order, r.commands.at(i).order);
    }
    EXPECT_EQ(to_text(again), text);
}

TEST(Replay, TheFieldReaderReadsEveryKindTheWayTheFormatWritesIt) {
    // `fields_of` is the ONE definition of what a payload carries: the writer asks it how many
    // numbers to write, the state hash asks it for the fixed-shape stream. Pinning the shape per
    // kind is what stops a kind from quietly borrowing another kind's arity -- which is exactly how
    // a verb table with a hand-written count went wrong.
    const OrderFields move = fields_of(orders::MoveTo{Fixed::from_int(3), Fixed::from_int(4)});
    EXPECT_EQ(move.position_count, 2U);
    EXPECT_EQ(move.positions.at(0), Fixed::from_int(3));
    EXPECT_EQ(move.positions.at(1), Fixed::from_int(4));
    EXPECT_EQ(move.positions.at(2), Fixed::zero()) << "a kind must not invent positions";
    EXPECT_EQ(move.positions.at(3), Fixed::zero());
    EXPECT_FALSE(move.has_target);

    const OrderFields guard = fields_of(orders::GuardAt{Fixed::from_int(5), Fixed::from_int(6)});
    EXPECT_EQ(guard.position_count, 2U);
    EXPECT_FALSE(guard.has_target);

    const OrderFields patrol = fields_of(orders::Patrol{Fixed::from_int(1), Fixed::from_int(2),
                                                        Fixed::from_int(7), Fixed::from_int(8)});
    EXPECT_EQ(patrol.position_count, 4U);
    EXPECT_EQ(patrol.positions.at(2), Fixed::from_int(7));
    EXPECT_EQ(patrol.positions.at(3), Fixed::from_int(8));
    EXPECT_FALSE(patrol.has_target);

    const OrderFields attack = fields_of(orders::Attack{UnitId{9}});
    EXPECT_EQ(attack.position_count, 0U) << "a shoot-this order carries no destination";
    EXPECT_TRUE(attack.has_target);
    EXPECT_EQ(attack.target, UnitId{9});

    const OrderFields focus = fields_of(orders::FocusFire{UnitId{11}});
    EXPECT_EQ(focus.position_count, 0U);
    EXPECT_TRUE(focus.has_target);
    EXPECT_EQ(focus.target, UnitId{11});
    EXPECT_EQ(focus.positions, attack.positions)
        << "the hash tells these apart by kind, hashed first";

    const OrderFields stop = fields_of(orders::Stop{});
    EXPECT_EQ(stop.position_count, 0U);
    EXPECT_FALSE(stop.has_target);
    EXPECT_EQ(stop.target, UnitId{});
    const std::array<Fixed, 4> nothing{};
    EXPECT_TRUE(stop.positions == nothing) << "a kind that carries nothing carries zeros";
}

TEST(Replay, AcceptsBothPositionSpellings) {
    const Replay raw = must_parse("# protekt replay v1\nseed 1\n0 move 1 3932160 1310720\n");
    EXPECT_EQ(move_of(raw).x, Fixed::from_int(60));
    EXPECT_EQ(move_of(raw).y, Fixed::from_raw(1310720));

    const Replay decimal = must_parse("# protekt replay v1\nseed 1\n0 move 1 24.5 20.25\n");
    EXPECT_EQ(move_of(decimal).x.raw, 24 * 65536 + 32768);
    EXPECT_EQ(move_of(decimal).y.raw, 20 * 65536 + 16384);

    const Replay negative = must_parse("# protekt replay v1\nseed 1\n0 move 1 -1.5 0\n");
    EXPECT_EQ(move_of(negative).x.raw, -(1 * 65536 + 32768));
}

TEST(Replay, ToleratesCommentsBlanksAndCrlf) {
    const Replay r = must_parse("# protekt replay v1\r\n\r\nseed 42\r\n# a comment\r\n\r\n"
                                "0 stop 1\r\n\n");
    EXPECT_EQ(r.seed, 42U);
    ASSERT_EQ(r.commands.size(), 1U);
    EXPECT_EQ(r.commands.at(0).order.kind(), OrderKind::Stop);
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
        Command{90, Order{UnitId{1}, orders::Stop{}}},
        Command{10, Order{UnitId{2}, orders::Stop{}}},
    };
    const Replay parsed = must_parse(to_text(r));
    for (std::size_t i = 1; i < parsed.commands.size(); ++i) {
        EXPECT_LE(parsed.commands[i - 1].tick, parsed.commands[i].tick);
    }
}

} // namespace
