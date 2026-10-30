#include "protekt/app/scenario_toml.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "protekt/app/args.hpp"

namespace protekt::app {

namespace {

using sim::Group;
using sim::UnitKind;

/// The scenario file's name for each kind: the enum's own name, lower case, words joined by an
/// underscore. The one table of these names; the parser reads it and its error message lists it.
struct KindName {
    std::string_view name;
    UnitKind kind;
};
constexpr std::array<KindName, static_cast<std::size_t>(sim::kUnitKindCount)> kKindNames{{
    {"tank", UnitKind::Tank},
    {"turret", UnitKind::Turret},
    {"factory", UnitKind::Factory},
    {"hover_tank", UnitKind::HoverTank},
    {"mortar_bike", UnitKind::MortarBike},
    {"rocket_tank", UnitKind::RocketTank},
    {"scavenger", UnitKind::Scavenger},
    {"recycler", UnitKind::Recycler},
    {"deployed_recycler", UnitKind::DeployedRecycler},
    {"scout", UnitKind::Scout},
    {"assault_tank", UnitKind::AssaultTank},
    {"walker", UnitKind::Walker},
    {"apc", UnitKind::Apc},
    {"bomber", UnitKind::Bomber},
    {"msl_scout", UnitKind::MslScout},
}};

/// The keys of a group, in the order a missing one is reported.
constexpr std::size_t kKind = 0;
constexpr std::size_t kCount = 1;
constexpr std::size_t kDx = 2;
/// kind, count, dx and dy. `dy` is the last key and is never compared, so it needs no index.
constexpr std::size_t kKeyCount = 4;
constexpr std::array<std::string_view, kKeyCount> kKeyNames{"kind", "count", "dx", "dy"};

std::string_view trim(std::string_view text) {
    constexpr std::string_view kBlank = " \t\r";
    const std::size_t first = text.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kBlank) - first + 1);
}

/// True when only blanks, or a comment, are left of the line.
bool rest_is_empty(std::string_view rest) {
    rest = trim(rest);
    return rest.empty() || rest.front() == '#';
}

std::string at_line(int line, const std::string& what) {
    return "line " + std::to_string(line) + ": " + what;
}

/// What a value was written as: the text, and whether it was in double quotes.
struct Value {
    std::string_view text{};
    bool quoted{false};
};

/// Reads the value after `=`: a double-quoted string, or a bare token. Nothing but blanks or a
/// comment may follow it. Returns an empty string on success, the reason otherwise.
std::string read_value(std::string_view raw, Value& value) {
    raw = trim(raw);
    if (!raw.empty() && raw.front() == '"') {
        const std::size_t close = raw.find('"', 1);
        if (close == std::string_view::npos) {
            return "a string is missing its closing quote";
        }
        if (!rest_is_empty(raw.substr(close + 1))) {
            return "unexpected text after the value";
        }
        value = Value{raw.substr(1, close - 1), true};
        return {};
    }
    const std::size_t end = raw.find_first_of(" \t#");
    const std::string_view token = raw.substr(0, end);
    if (end != std::string_view::npos && !rest_is_empty(raw.substr(end))) {
        return "unexpected text after the value";
    }
    value = Value{token, false};
    return {};
}

std::string describe(const Value& value) {
    if (value.quoted) {
        return "the string \"" + std::string(value.text) + "\"";
    }
    return value.text.empty() ? "nothing" : "'" + std::string(value.text) + "'";
}

std::string kind_names() {
    std::string out;
    for (const KindName& entry : kKindNames) {
        out += out.empty() ? "" : ", ";
        out += entry.name;
    }
    return out;
}

/// One group being read: what it has said so far, and where it opened.
struct Pending {
    Group group{};
    std::array<bool, kKeyCount> seen{};
    int header_line{0};
};

/// Reading state for one file. Each member function returns an empty string on success, otherwise
/// the whole error, line number included.
class Reader {
public:
    std::string line(int number, std::string_view text);
    std::string finish();
    /// The groups read so far; call once, after finish().
    sim::ScenarioData take() { return std::move(data_); }

private:
    std::string header(int number, std::string_view text);
    std::string assignment(int number, std::string_view text);
    std::string store(int number, std::size_t key, const Value& value);
    std::string close_group();

    sim::ScenarioData data_{};
    Pending pending_{};
    bool open_{false};
    int side_units_{0};
};

std::string Reader::line(int number, std::string_view text) {
    text = trim(text);
    if (text.empty() || text.front() == '#') {
        return {};
    }
    return text.front() == '[' ? header(number, text) : assignment(number, text);
}

std::string Reader::header(int number, std::string_view text) {
    const std::string_view name = trim(text.substr(0, text.find('#')));
    if (name != "[[group]]") {
        return at_line(number,
                       "unknown table '" + std::string(name) + "': the only table is [[group]]");
    }
    if (std::string error = close_group(); !error.empty()) {
        return error;
    }
    pending_ = Pending{};
    pending_.header_line = number;
    open_ = true;
    return {};
}

std::string Reader::assignment(int number, std::string_view text) {
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos) {
        return at_line(number, "expected 'key = value' or a [[group]] header");
    }
    if (!open_) {
        return at_line(number, "a key outside a [[group]]: open one with [[group]] first");
    }
    const std::string_view key = trim(text.substr(0, equals));
    if (key == "weapon") {
        return at_line(number, "'weapon' is not a field: a weapon is a property of the kind, and "
                               "a per-unit loadout is not built yet");
    }
    if (key == "key") {
        return at_line(number, "'key' is not a field: a group's function key follows its order in "
                               "the file");
    }
    std::size_t index = kKeyCount;
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        if (key == kKeyNames.at(i)) {
            index = i;
        }
    }
    if (index == kKeyCount) {
        return at_line(number, "unknown key '" + std::string(key)
                                   + "': a group has kind, count, dx and dy");
    }
    if (pending_.seen.at(index)) {
        return at_line(number, "'" + std::string(key) + "' is given twice in one group");
    }
    Value value;
    if (const std::string error = read_value(text.substr(equals + 1), value); !error.empty()) {
        return at_line(number, "'" + std::string(key) + "': " + error);
    }
    return store(number, index, value);
}

std::string Reader::store(int number, std::size_t key, const Value& value) {
    const std::string name(kKeyNames.at(key));
    if (key == kKind) {
        if (!value.quoted) {
            return at_line(number, "'kind' wants a double-quoted string, got " + describe(value));
        }
        for (const KindName& entry : kKindNames) {
            if (entry.name == value.text) {
                pending_.group.kind = entry.kind;
                pending_.seen.at(key) = true;
                return {};
            }
        }
        return at_line(number, "unknown kind '" + std::string(value.text) + "': the kinds are "
                                   + kind_names());
    }
    int number_value = 0;
    if (value.quoted || !parse_number(value.text, number_value)) {
        return at_line(number, "'" + name + "' wants an integer, got " + describe(value));
    }
    const int low = key == kCount ? 1 : -sim::kMaxOffset;
    const int high = key == kCount ? sim::kMaxGroupCount : sim::kMaxOffset;
    if (number_value < low || number_value > high) {
        return at_line(number, "'" + name + "' is " + std::to_string(number_value) + ", outside "
                                   + std::to_string(low) + ".." + std::to_string(high));
    }
    int& field = key == kCount ? pending_.group.count
                 : key == kDx  ? pending_.group.dx
                               : pending_.group.dy;
    field = number_value;
    pending_.seen.at(key) = true;
    return {};
}

/// Ends the group being read, if any: every key must have been said, and the side's total must
/// still fit. Both are reported at the line the group opened on.
std::string Reader::close_group() {
    if (!open_) {
        return {};
    }
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        if (!pending_.seen.at(i)) {
            return at_line(pending_.header_line,
                           "this group has no '" + std::string(kKeyNames.at(i)) + "'");
        }
    }
    side_units_ += pending_.group.count;
    if (side_units_ > sim::kMaxSideUnits) {
        return at_line(pending_.header_line,
                       "this group takes a side to " + std::to_string(side_units_)
                           + " units, and the most is " + std::to_string(sim::kMaxSideUnits));
    }
    data_.groups.push_back(pending_.group);
    open_ = false;
    return {};
}

std::string Reader::finish() {
    if (std::string error = close_group(); !error.empty()) {
        return error;
    }
    if (data_.groups.empty()) {
        return "the file has no [[group]]: a scenario needs units, 1.."
               + std::to_string(sim::kMaxSideUnits) + " a side";
    }
    return {};
}

} // namespace

ScenarioParse load_scenario_data(std::string_view text) {
    ScenarioParse parse;
    Reader reader;
    int number = 0;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        ++number;
        parse.error = reader.line(number, text.substr(0, newline));
        if (!parse.error.empty()) {
            return parse;
        }
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    }
    parse.error = reader.finish();
    parse.ok = parse.error.empty();
    if (parse.ok) {
        parse.data = reader.take();
    }
    return parse;
}

ScenarioParse load_scenario_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ScenarioParse parse;
        parse.error = "cannot read '" + path + "'";
        return parse;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return load_scenario_data(text.str());
}

} // namespace protekt::app
