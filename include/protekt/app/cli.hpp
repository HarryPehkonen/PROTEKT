#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace protekt::app {

/// The command line, as a function rather than a main(), so the suite can drive it with
/// stringstreams and assert on the exact output. sim_cli.cpp is the only caller.
///
///   protekt-sim [--seed N] [--ticks N] [--commander none|scripted]
///               [--replay FILE] [--replay-out FILE] [--status-period N]
///
/// Defaults: seed 42, 900 ticks, --commander scripted (driving BOTH sides, so the battle is a
/// real fight), status period 60.
/// Exits 0 on success, 2 on a usage error, 3 on a replay that will not parse. The battle log goes
/// to `out`; problems go to `err` as `protekt-sim: <reason>`.
int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

} // namespace protekt::app
