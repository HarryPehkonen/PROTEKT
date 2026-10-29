#pragma once

#include <string>
#include <string_view>

#include "protekt/sim/scenario.hpp"

namespace protekt::app {

/// The outcome of reading a scenario: the data when `ok`, otherwise `error` says why, in the form
/// "line N: ..." (a whole-file fault names the bound instead of a line).
struct ScenarioParse {
    sim::ScenarioData data{};
    std::string error{};
    bool ok{false};
};

/// Reads a scenario from TOML text. This is a small, STRICT SUBSET of TOML, not a TOML parser:
///
///   * `#` comments and blank lines, anywhere;
///   * the one table header `[[group]]`, repeated once per group; groups spawn in the order
///   written;
///   * the keys `kind` (a double-quoted string, no escapes), and `count`, `dx`, `dy` (bare integers
///     with an optional leading `-`), one per line as `key = value`, each exactly once per group;
///   * `kind` is a UnitKind in lower case with underscores: tank, turret, factory, hover_tank,
///     mortar_bike, rocket_tank, scavenger, recycler,
///     deployed_recycler or scout;
///   * `count` is 1..kMaxGroupCount, `dx` and `dy` are within +-kMaxOffset, and the groups total
///     at most kMaxSideUnits units a side.
///
/// Anything else is refused with the line it is on: an unknown table or key, a missing or doubled
/// key, a value of the wrong type, a nested table or an array. `weapon` and `key` are refused with
/// their reason, not as misspellings: a weapon is a property of the kind (a per-unit loadout is not
/// built), and the function key follows the order of the groups. Nothing is skipped, guessed or
/// defaulted.
[[nodiscard]] ScenarioParse load_scenario_data(std::string_view text);

/// load_scenario_data over the contents of the file at `path`. A file that cannot be read is an
/// error like any other.
[[nodiscard]] ScenarioParse load_scenario_file(const std::string& path);

} // namespace protekt::app
