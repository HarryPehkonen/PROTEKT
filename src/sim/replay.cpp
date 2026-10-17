#include "protekt/sim/replay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::sim {

namespace {

constexpr std::string_view kHeader = "# protekt replay v1";
constexpr std::size_t kMaxTokens = 8;

struct VerbInfo {
    std::string_view name;
    OrderKind kind;
    int position_args; ///< number of position values (x y [x2 y2])
    bool takes_target;
};

constexpr std::array<VerbInfo, kOrderKindCount> kVerbs{{
    {"move", OrderKind::MoveTo, 2, false},
    {"guard", OrderKind::GuardAt, 2, false},
    {"patrol", OrderKind::Patrol, 4, false},
    {"attack", OrderKind::Attack, 0, true},
    {"stop", OrderKind::Stop, 0, false},
}};

const VerbInfo* find_verb(OrderKind kind) {
    for (const VerbInfo& v : kVerbs) {
        if (v.kind == kind) {
            return &v;
        }
    }
    return nullptr;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (is_blank(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (is_blank(s.back()) || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

/// Unsigned decimal, digits only, no sign, no overflow past `max`.
bool parse_unsigned(std::string_view s, std::uint64_t max, std::uint64_t& out) {
    if (s.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : s) {
        if (!is_digit(c)) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (max - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

/// A position: a plain integer is a raw Q16.16 value; a literal containing '.' is a tile value
/// with 1..3 fractional digits. Integer arithmetic only.
bool parse_position(std::string_view s, Fixed& out) {
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    const std::size_t dot = s.find('.');
    std::int64_t raw = 0;
    if (dot == std::string_view::npos) {
        std::uint64_t magnitude = 0;
        if (!parse_unsigned(s, std::uint64_t{1} << 31, magnitude)) {
            return false;
        }
        raw = static_cast<std::int64_t>(magnitude);
    } else {
        const std::string_view whole = s.substr(0, dot);
        const std::string_view frac = s.substr(dot + 1);
        std::uint64_t whole_value = 0;
        std::uint64_t frac_value = 0;
        if (frac.empty() || frac.size() > 3 || !parse_unsigned(whole, 32768, whole_value)
            || !parse_unsigned(frac, 999, frac_value)) {
            return false;
        }
        std::uint64_t scale = 1;
        for (std::size_t i = 0; i < frac.size(); ++i) {
            scale *= 10;
        }
        raw = static_cast<std::int64_t>(whole_value * Fixed::kOne
                                        + (frac_value * Fixed::kOne) / scale);
    }
    if (negative) {
        raw = -raw;
    }
    if (raw < Fixed::kMinRaw || raw > Fixed::kMaxRaw) {
        return false;
    }
    out = Fixed::from_raw(static_cast<std::int32_t>(raw));
    return true;
}

bool fail(std::string* error, std::size_t line_no, const char* reason) {
    if (error != nullptr) {
        *error = "line " + std::to_string(line_no) + ": " + reason;
    }
    return false;
}

bool parse_command_line(const std::array<std::string_view, kMaxTokens>& tok, std::size_t count,
                        Command& out, const char*& reason) {
    std::uint64_t tick = 0;
    if (!parse_unsigned(tok[0], 0xFFFFFFFFULL, tick)) {
        reason = "bad tick";
        return false;
    }
    const VerbInfo* verb = nullptr;
    for (const VerbInfo& v : kVerbs) {
        if (v.name == tok[1]) {
            verb = &v;
        }
    }
    if (verb == nullptr) {
        reason = "unknown verb";
        return false;
    }
    const std::size_t expected
        = 3 + static_cast<std::size_t>(verb->position_args) + (verb->takes_target ? 1U : 0U);
    if (count != expected) {
        reason = "wrong number of arguments";
        return false;
    }
    std::uint64_t unit = 0;
    if (!parse_unsigned(tok[2], 0xFFFFFFFFULL, unit) || unit == 0) {
        reason = "bad unit id";
        return false;
    }
    Command cmd{};
    cmd.tick = static_cast<Tick>(tick);
    cmd.order.kind = verb->kind;
    cmd.order.unit = UnitId{static_cast<std::uint32_t>(unit)};
    const std::array<Fixed*, 4> slots{&cmd.order.x, &cmd.order.y, &cmd.order.x2, &cmd.order.y2};
    for (int i = 0; i < verb->position_args; ++i) {
        if (!parse_position(tok.at(3 + static_cast<std::size_t>(i)),
                            *slots.at(static_cast<std::size_t>(i)))) {
            reason = "bad position";
            return false;
        }
    }
    if (verb->takes_target) {
        std::uint64_t target = 0;
        if (!parse_unsigned(tok[3], 0xFFFFFFFFULL, target) || target == 0) {
            reason = "bad target id";
            return false;
        }
        cmd.order.target = UnitId{static_cast<std::uint32_t>(target)};
    }
    out = cmd;
    return true;
}

} // namespace

std::string to_text(const Replay& replay) {
    std::vector<Command> sorted = replay.commands;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Command& a, const Command& b) { return a.tick < b.tick; });
    std::string text(kHeader);
    text += "\nseed ";
    text += std::to_string(replay.seed);
    text += '\n';
    for (const Command& cmd : sorted) {
        const VerbInfo* verb = find_verb(cmd.order.kind);
        if (verb == nullptr) {
            continue; // not a real order kind: there is no spelling for it
        }
        text += std::to_string(cmd.tick);
        text += ' ';
        text += verb->name;
        text += ' ';
        text += std::to_string(cmd.order.unit.value);
        const std::array<Fixed, 4> positions{cmd.order.x, cmd.order.y, cmd.order.x2, cmd.order.y2};
        for (int i = 0; i < verb->position_args; ++i) {
            text += ' ';
            text += std::to_string(positions.at(static_cast<std::size_t>(i)).raw);
        }
        if (verb->takes_target) {
            text += ' ';
            text += std::to_string(cmd.order.target.value);
        }
        text += '\n';
    }
    return text;
}

std::optional<Replay> parse(std::string_view text, std::string* error) {
    Replay replay{};
    bool have_header = false;
    bool have_seed = false;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        ++line_no;

        if (!have_header) {
            if (line != kHeader) {
                fail(error, line_no, "missing header '# protekt replay v1'");
                return std::nullopt;
            }
            have_header = true;
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::array<std::string_view, kMaxTokens> tok{};
        std::size_t count = 0;
        std::string_view rest = line;
        while (!rest.empty()) {
            const std::size_t start = rest.find_first_not_of(" \t");
            if (start == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(start);
            std::size_t len = 0;
            while (len < rest.size() && !is_blank(rest[len])) {
                ++len;
            }
            if (count == kMaxTokens) {
                fail(error, line_no, "too many fields");
                return std::nullopt;
            }
            tok.at(count++) = rest.substr(0, len);
            rest.remove_prefix(len);
        }

        if (tok[0] == "seed") {
            if (have_seed) {
                fail(error, line_no, "duplicate seed");
                return std::nullopt;
            }
            if (!replay.commands.empty()) {
                fail(error, line_no, "seed after the first command");
                return std::nullopt;
            }
            std::uint64_t seed = 0;
            if (count != 2 || !parse_unsigned(tok[1], ~std::uint64_t{0}, seed)) {
                fail(error, line_no, "bad seed");
                return std::nullopt;
            }
            replay.seed = seed;
            have_seed = true;
            continue;
        }
        if (!have_seed) {
            fail(error, line_no, "command before seed");
            return std::nullopt;
        }
        if (count < 3) {
            fail(error, line_no, "too few fields");
            return std::nullopt;
        }
        Command cmd{};
        const char* reason = "bad command";
        if (!parse_command_line(tok, count, cmd, reason)) {
            fail(error, line_no, reason);
            return std::nullopt;
        }
        if (!replay.commands.empty() && cmd.tick < replay.commands.back().tick) {
            fail(error, line_no, "ticks must not decrease");
            return std::nullopt;
        }
        replay.commands.push_back(cmd);
    }
    if (!have_header) {
        fail(error, 1, "empty input: missing header");
        return std::nullopt;
    }
    if (!have_seed) {
        fail(error, line_no, "missing seed");
        return std::nullopt;
    }
    return replay;
}

std::uint64_t run(const Replay& replay) {
    std::vector<Command> sorted = replay.commands;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Command& a, const Command& b) { return a.tick < b.tick; });
    Sim sim = Sim::create(skirmish(replay.seed));
    const std::uint64_t last = sorted.empty() ? 0 : sorted.back().tick;
    const std::uint64_t steps = std::max<std::uint64_t>(1, last + 1);
    std::size_t next = 0;
    for (std::uint64_t t = 0; t < steps; ++t) {
        while (next < sorted.size() && sorted[next].tick <= t) {
            sim.enqueue(sorted[next].order);
            ++next;
        }
        sim.step();
    }
    return sim.state_hash();
}

} // namespace protekt::sim
