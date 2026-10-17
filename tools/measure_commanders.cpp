#include "protekt/ai/utility.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Measures one commander against another over a fixed set of seeds, and prints what happened.
///
/// "This AI is better" is not a unit test, and it is not a claim a chat message can carry: it is a
/// number produced by running both, including the seeds where the new one does worse. So the arms
/// include a PLACEBO -- the enemy side receives no orders at all -- because the obvious metric,
/// "fewer units lost", is won by a commander that does nothing. Only the asymmetry survives that
/// check: how many units did it kill for how many of its own it lost.
namespace {

using protekt::ai::ICommander;
using protekt::ai::ScriptedCommander;
using protekt::ai::UtilityCommander;
using protekt::ai::UtilityTuning;
using protekt::sim::BattleEvent;
using protekt::sim::EventKind;

enum class Role : std::uint8_t { None = 0, Scripted = 1, Utility = 2 };

struct Tally {
    int player_lost{};
    int enemy_lost{};
    int first_contact{-1};
    int first_kill{-1};
};

/// Which side owned a unit, read from the log's own status lines. The last line naming the unit
/// wins, and a unit that stops appearing was killed -- the log is the only record, so this reads it
/// rather than reaching into the simulation for a truth the recording would not have.
[[nodiscard]] int side_of(const std::string& text, std::uint32_t id) {
    const std::string needle = "status unit " + std::to_string(id) + " ";
    int side = -1;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        const std::size_t marker = text.find(" side ", at);
        if (marker != std::string::npos) {
            side = text.compare(marker + 6, 6, "player") == 0 ? 0 : 1;
        }
    }
    return side;
}

[[nodiscard]] std::unique_ptr<ICommander> make(Role role, ScriptedCommander& scripted,
                                               UtilityCommander& utility) {
    switch (role) {
    case Role::Scripted:
        return std::make_unique<ScriptedCommander>(scripted);
    case Role::Utility:
        return std::make_unique<UtilityCommander>(utility);
    case Role::None:
        break;
    }
    return nullptr;
}

[[nodiscard]] Tally run(unsigned seed, unsigned ticks, Role player_role, Role enemy_role) {
    ScriptedCommander scripted_player{15};
    ScriptedCommander scripted_enemy{15};
    UtilityCommander utility_player{UtilityTuning{}};
    UtilityCommander utility_enemy{UtilityTuning{}};
    std::unique_ptr<ICommander> player = make(player_role, scripted_player, utility_player);
    std::unique_ptr<ICommander> enemy = make(enemy_role, scripted_enemy, utility_enemy);

    const protekt::sim::BattleLog log = protekt::sim::run_battle(protekt::sim::skirmish(seed), {},
                                                                 ticks, player.get(), enemy.get());
    const std::string text = log.to_text(1000000);
    Tally tally{};
    for (const BattleEvent& event : log.events) {
        if (event.kind == EventKind::Fired && tally.first_contact < 0) {
            tally.first_contact = static_cast<int>(event.tick);
        }
        if (event.kind != EventKind::Killed) {
            continue;
        }
        if (tally.first_kill < 0) {
            tally.first_kill = static_cast<int>(event.tick);
        }
        const int victim = side_of(text, event.subject.value);
        if (victim == 0) {
            ++tally.player_lost;
        } else if (victim == 1) {
            ++tally.enemy_lost;
        }
    }
    return tally;
}

struct Arm {
    const char* name;
    Role player;
    Role enemy;
};

constexpr std::array<unsigned, 10> kSeeds{7, 11, 42, 99, 123, 500, 777, 1024, 31337, 65535};

constexpr std::array<Arm, 5> kArms{{
    {"placebo enemy -- the enemy side gets NO orders at all", Role::Scripted, Role::None},
    {"scripted enemy", Role::Scripted, Role::Scripted},
    {"utility enemy", Role::Scripted, Role::Utility},
    {"utility PLAYER against a scripted enemy", Role::Utility, Role::Scripted},
    {"utility against utility", Role::Utility, Role::Utility},
}};

} // namespace

int run_measure_commanders(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err) {
    unsigned ticks = 2400;
    if (!args.empty()) {
        const std::string_view arg = args.front();
        const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), ticks);
        if (parsed.ec != std::errc{}) {
            err << "usage: protekt-measure [ticks]  (default 2400)\n";
            return 2;
        }
    }
    out << "commander measurement, " << ticks << " ticks, " << kSeeds.size()
        << " seeds, scenario skirmish(seed)\n\n";
    for (const Arm& arm : kArms) {
        int player_lost = 0;
        int enemy_lost = 0;
        int contacts = 0;
        out << arm.name << '\n';
        for (const unsigned seed : kSeeds) {
            const Tally tally = run(seed, ticks, arm.player, arm.enemy);
            out << "  seed " << seed << "  units lost: player " << tally.player_lost << ", enemy "
                << tally.enemy_lost << "   first contact "
                << (tally.first_contact < 0 ? "never" : std::to_string(tally.first_contact))
                << "   first kill "
                << (tally.first_kill < 0 ? "never" : std::to_string(tally.first_kill)) << '\n';
            player_lost += tally.player_lost;
            enemy_lost += tally.enemy_lost;
            contacts += tally.first_contact >= 0 ? 1 : 0;
        }
        out << "  TOTAL: player lost " << player_lost << ", enemy lost " << enemy_lost
            << ", seeds where anything was fired at all: " << contacts << '/' << std::size(kSeeds)
            << "\n\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    std::vector<std::string_view> args;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        args.emplace_back(raw[i]);
    }
    return run_measure_commanders(args, std::cout, std::cerr);
}
