#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/sim/orders.hpp"

namespace protekt::sim {

/// A whole battle in a file: the seed (which determines the world AND the starting units, see
/// scenario.hpp) plus the commands in tick order.
struct Replay {
    std::uint64_t seed{};
    std::vector<Command> commands{};
};

/// The canonical text form. FROZEN, because it is also the fuzzer's input language:
///
///   # protekt replay v1
///   seed 42
///   100 move 3 3932160 1310720
///   100 guard 4 1966080 1966080
///   250 attack 3 7
///   300 stop 3
///
/// Rules:
///   - line 1 is exactly `# protekt replay v1`; any OTHER line starting with '#' is a comment
///     and is ignored, as are blank lines and trailing whitespace (CRLF tolerated);
///   - exactly one `seed <u64>` line, and it must come before the first command line;
///   - one command per line: `<tick> <verb> <unit> [args...]`;
///   - verbs and their arguments:
///       move <unit> <x> <y>        guard <unit> <x> <y>
///       patrol <unit> <x> <y> <x2> <y2>
///       attack <unit> <target>     stop <unit>     focusfire <unit> <target>
///   - numbers are decimal. Ticks, units and targets are plain integers. POSITIONS have two
///     accepted spellings: a plain integer is a raw Q16.16 value, and a literal containing '.'
///     is a tile value with up to 3 fractional digits, parsed with integer arithmetic only
///     (never a float). The EMITTER always writes the raw form -- that is the canonical spelling,
///     so to_text(parse(to_text(r))) == to_text(r) byte for byte;
///   - commands must be in NON-DECREASING tick order.
///
/// Writes the canonical form. Deterministic, and independent of the locale.
[[nodiscard]] std::string to_text(const Replay& replay);

/// Parses the above. Returns nothing and fills `error` with a one-line reason on any of: missing
/// or duplicated header, missing seed, a seed after the first command, an unknown verb, wrong
/// arity, a number that is not a number, a value out of range, a non-decreasing-tick violation.
/// NEVER throws and never reads out of bounds: this is the fuzzer's entry point, so a malformed
/// replay must come back as an error, not as a crash.
[[nodiscard]] std::optional<Replay> parse(std::string_view text, std::string* error = nullptr);

/// Builds the standard skirmish for the replay's seed, applies its commands at their ticks, and
/// returns the final state_hash(). Steps `max(1U, last_command_tick + 1)` ticks, so a replay whose
/// last command is at tick 899 finishes with tick() == 900. An empty command list runs one tick.
[[nodiscard]] std::uint64_t run(const Replay& replay);

} // namespace protekt::sim
