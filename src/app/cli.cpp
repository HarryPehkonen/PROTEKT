#include "protekt/app/cli.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "protekt/ai/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/replay.hpp"
#include "protekt/sim/scenario.hpp"

namespace protekt::app {

namespace {

constexpr std::uint64_t kMaxTicks = 1'000'000; ///< a sanity bound: the log keeps per-tick state

struct Options {
    std::uint64_t seed = 42;
    std::uint32_t ticks = 900;
    std::uint32_t status_period = 60;
    bool scripted = true;
    std::optional<std::string> replay_in{};
    std::optional<std::string> replay_out{};
};

/// Digits only: no sign, no whitespace, no overflow past `max`.
std::optional<std::uint64_t> parse_number(std::string_view s, std::uint64_t max) {
    if (s.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (max - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

/// Parses the arguments; on failure returns the reason.
std::optional<std::string> parse_args(std::span<const std::string_view> args, Options& opt) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        const bool known = flag == "--seed" || flag == "--ticks" || flag == "--commander"
                           || flag == "--replay" || flag == "--replay-out"
                           || flag == "--status-period";
        if (!known) {
            return "unknown argument '" + std::string(flag) + "'";
        }
        if (i + 1 >= args.size()) {
            return "missing value for " + std::string(flag);
        }
        const std::string_view value = args[++i];
        if (flag == "--seed") {
            const auto n = parse_number(value, ~std::uint64_t{0});
            if (!n) {
                return "bad --seed value '" + std::string(value) + "'";
            }
            opt.seed = *n;
        } else if (flag == "--ticks") {
            const auto n = parse_number(value, kMaxTicks);
            if (!n) {
                return "bad --ticks value '" + std::string(value) + "'";
            }
            opt.ticks = static_cast<std::uint32_t>(*n);
        } else if (flag == "--status-period") {
            const auto n = parse_number(value, 0xFFFFFFFFULL);
            if (!n) {
                return "bad --status-period value '" + std::string(value) + "'";
            }
            opt.status_period = static_cast<std::uint32_t>(*n);
        } else if (flag == "--commander") {
            if (value != "none" && value != "scripted") {
                return "bad --commander value '" + std::string(value) + "' (none|scripted)";
            }
            opt.scripted = value == "scripted";
        } else if (flag == "--replay") {
            opt.replay_in = std::string(value);
        } else {
            opt.replay_out = std::string(value);
        }
    }
    return std::nullopt;
}

/// Passes orders through and notes every one with the tick it was issued on, so a live battle can
/// be written out as a replay.
class RecordingCommander final : public ai::ICommander {
public:
    RecordingCommander(ai::ICommander& inner, std::vector<sim::Command>& sink)
        : inner_(inner), sink_(sink) {}

    void think(const ai::Observation& observation, std::vector<sim::Order>& out) override {
        const std::size_t first = out.size();
        inner_.think(observation, out);
        for (std::size_t i = first; i < out.size(); ++i) {
            sink_.push_back(sim::Command{observation.tick, out[i]});
        }
    }

private:
    ai::ICommander& inner_;
    std::vector<sim::Command>& sink_;
};

/// The log without its status lines (what --status-period 0 asks for).
std::string without_status(const std::string& text) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == std::string::npos ? text.size() : nl + 1;
        const std::string_view line(text.data() + pos, end - pos);
        if (line.find(" status unit ") == std::string_view::npos) {
            out.append(line);
        }
        pos = end;
    }
    return out;
}

} // namespace

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    Options opt;
    if (const auto problem = parse_args(args, opt)) {
        err << "protekt-sim: " << *problem << '\n';
        return 2;
    }

    sim::Replay replay{};
    sim::BattleLog log;
    if (opt.replay_in) {
        std::ifstream in(*opt.replay_in, std::ios::binary);
        if (!in) {
            err << "protekt-sim: cannot read replay '" << *opt.replay_in << "'\n";
            return 3;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::string reason;
        auto parsed = sim::parse(text, &reason);
        if (!parsed) {
            err << "protekt-sim: bad replay '" << *opt.replay_in << "': " << reason << '\n';
            return 3;
        }
        replay = std::move(*parsed);
        // The replay IS the battle: its commands stand in for the commander.
        log = sim::run_battle(sim::skirmish(replay.seed), replay.commands, opt.ticks, nullptr,
                              nullptr);
    } else {
        replay.seed = opt.seed;
        ai::ScriptedCommander scripted_player;
        ai::ScriptedCommander scripted_enemy;
        // BOTH sides are recorded: a replay missing the enemy's orders replays a different battle
        // -- one side drives while the other stands still.
        RecordingCommander player_recorder(scripted_player, replay.commands);
        RecordingCommander enemy_recorder(scripted_enemy, replay.commands);
        if (opt.scripted) {
            log = sim::run_battle(sim::skirmish(opt.seed), {}, opt.ticks, &player_recorder,
                                  &enemy_recorder);
        } else {
            log = sim::run_battle(sim::skirmish(opt.seed), {}, opt.ticks, nullptr, nullptr);
        }
    }

    const std::string text = log.to_text(opt.status_period);
    out << (opt.status_period == 0 ? without_status(text) : text);

    if (opt.replay_out) {
        std::ofstream file(*opt.replay_out, std::ios::binary | std::ios::trunc);
        file << sim::to_text(replay);
        if (!file) {
            err << "protekt-sim: cannot write replay '" << *opt.replay_out << "'\n";
            return 1;
        }
    }
    return 0;
}

} // namespace protekt::app
