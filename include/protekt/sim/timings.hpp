#pragma once

#include <cstddef>
#include <cstdint>

namespace protekt::sim {

/// N7: where one `Sim::step` spends its time, in integer NANOSECONDS (the sim is integer: no float
/// or double lives under `sim/`, Rule 13). A MEASUREMENT hook and nothing else: a step given no
/// `StepTimings` reads no clock, and a step given one computes exactly the same state (the hook
/// only reads `std::chrono::steady_clock` and writes here).
///
/// The phases are the step's own, in order, and they tile the step: `orders + movement + bolts
/// + fire + rest == total` (to the clock's resolution; the tool prints both so it can be seen).
/// `steer` is INSIDE `movement`, `hit_tests` INSIDE `bolts`, `line` and `probe` INSIDE `fire`, so
/// they are not added to the sum. (A clock read per call costs tens of nanoseconds: `line`, called
/// many times, carries that overhead and so reads high; `probe` flights are microseconds each and
/// do not.)
struct StepTimings {
    std::int64_t orders_ns{};   ///< 1. the queued orders applied
    std::int64_t movement_ns{}; ///< 2. deploy, scavenge, steer and move, every unit
    std::int64_t steer_ns{};    ///<    inside movement: `steer` alone (goals, routes, avoidance)
    std::int64_t bolts_ns{};    ///< 3. `advance_projectiles`: every bolt and shell in the air
    std::int64_t
        hit_tests_ns{};     ///<    inside bolts: the unit and building hit tests, per projectile
    std::int64_t fire_ns{}; ///< 4. every gun: target search, line of sight, launch
    std::int64_t line_ns{}; ///<    inside fire: `has_line`, the line-of-sight test, over all calls
    std::int64_t
        probe_ns{}; ///<    inside fire: `shot_hurts_friend`, the friendly-fire probe flight
    std::size_t line_calls{};     ///< how many `has_line` calls this step made
    std::size_t probe_calls{};    ///< how many probe flights this step flew
    std::size_t probe_refused{};  ///< ...of which found a friend in the lane (the gun declined)
    std::size_t line_blocked{};   ///< ...`has_line` calls that found NO line
    std::int64_t rest_ns{};       ///< 4b..7: lungs, repair, wrecks, store, build queues
    std::int64_t total_ns{};      ///< the whole step
    std::size_t projectiles_in{}; ///< bolts and shells in the air when the step began
    std::size_t units_alive{};    ///< living units (buildings included) when the step began
};

} // namespace protekt::sim
