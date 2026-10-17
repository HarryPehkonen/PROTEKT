#include "protekt/sim/observer.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <string>

namespace protekt::sim {

namespace {

// BattleLog is frozen at {ticks, seed, events, final_hash}, so the unit states that the status
// lines print have to live in `events` too. Two private event kinds past the public enum carry
// them: a "status position" event (actor = unit, subject = raw x, subject_hp = hp) is always
// followed by a "status attributes" event (actor = unit, subject = raw y, subject_hp = kind |
// side << 4 | stance << 8). They are recorded only when a unit's state changed since its last
// record, and to_text() replays them to know every unit's state at any tick, whatever status
// period it is asked for.
constexpr EventKind kStatusPos = static_cast<EventKind>(3);
constexpr EventKind kStatusAttr = static_cast<EventKind>(4);

bool is_status(EventKind kind) noexcept { return kind == kStatusPos || kind == kStatusAttr; }

struct UnitState {
    std::int32_t x{};
    std::int32_t y{};
    std::uint16_t hp{};
    std::uint16_t attrs{};
};

std::uint16_t pack_attrs(const Unit& u) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned>(u.kind)
                                      | (static_cast<unsigned>(u.side) << 4)
                                      | (static_cast<unsigned>(u.stance) << 8));
}

UnitState state_of(const Unit& u) noexcept {
    return UnitState{u.x.raw, u.y.raw, u.hp, pack_attrs(u)};
}

bool same(const UnitState& a, const UnitState& b) noexcept {
    return a.x == b.x && a.y == b.y && a.hp == b.hp && a.attrs == b.attrs;
}

UnitKind kind_of(std::uint16_t attrs) noexcept { return static_cast<UnitKind>(attrs & 0xFU); }
Side side_of(std::uint16_t attrs) noexcept { return static_cast<Side>((attrs >> 4) & 0xFU); }
Stance stance_of(std::uint16_t attrs) noexcept { return static_cast<Stance>((attrs >> 8) & 0xFFU); }

std::int64_t dist2(const Unit& a, const Unit& b) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(a.x.raw) - b.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(a.y.raw) - b.y.raw;
    return dx * dx + dy * dy;
}

/// Records the units whose state differs from the last record (everything, the first time).
void record_status(const Sim& sim, std::vector<UnitState>& last, std::vector<bool>& seen,
                   BattleLog& log) {
    const std::vector<Unit>& units = sim.units();
    last.resize(units.size());
    seen.resize(units.size(), false);
    for (std::size_t i = 0; i < units.size(); ++i) {
        const UnitState now = state_of(units[i]);
        if (seen[i] && same(last[i], now)) {
            continue;
        }
        seen[i] = true;
        last[i] = now;
        log.events.push_back(BattleEvent{sim.tick(), kStatusPos, units[i].id,
                                         UnitId{static_cast<std::uint32_t>(now.x)}, now.hp});
        log.events.push_back(BattleEvent{sim.tick(), kStatusAttr, units[i].id,
                                         UnitId{static_cast<std::uint32_t>(now.y)}, now.attrs});
    }
}

/// Reconstructs who shot whom during the step that took `before` to `sim`'s current state. A unit
/// fired this tick exactly when its cooldown went UP (a reload only ever counts down); its victim
/// is found with the sim's own targeting rule, applied in ascending id order against the hit
/// points as they stood before the step, so shots that kill are attributed to the right shooter.
void record_fire(const std::vector<Unit>& before, const Sim& sim, Tick tick, BattleLog& log) {
    const std::vector<Unit>& after = sim.units();
    if (before.size() != after.size()) {
        return;
    }
    std::vector<std::uint16_t> hp(before.size());
    std::vector<bool> alive(before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        hp[i] = before[i].hp;
        alive[i] = before[i].alive;
    }
    for (std::size_t i = 0; i < after.size(); ++i) {
        const Unit& shooter = after[i];
        if (!before[i].alive || shooter.cooldown <= before[i].cooldown) {
            continue;
        }
        const UnitProfile prof = profile(shooter.kind);
        const std::int64_t range = prof.weapon_range.raw;
        std::size_t best = after.size();
        if (shooter.target.valid()) { // only an Attack order leaves a target set
            for (std::size_t j = 0; j < after.size(); ++j) {
                if (after[j].id == shooter.target) {
                    if (alive[j] && after[j].side != shooter.side
                        && dist2(shooter, after[j]) <= range * range) {
                        best = j;
                    }
                    break;
                }
            }
        }
        if (best == after.size()) {
            std::int64_t best_d2 = range * range + 1;
            for (std::size_t j = 0; j < after.size(); ++j) {
                if (!alive[j] || after[j].side == shooter.side) {
                    continue;
                }
                const std::int64_t d2 = dist2(shooter, after[j]);
                if (d2 <= range * range && d2 < best_d2) {
                    best = j;
                    best_d2 = d2;
                }
            }
        }
        if (best == after.size()) {
            continue;
        }
        hp[best] = hp[best] > prof.damage ? static_cast<std::uint16_t>(hp[best] - prof.damage)
                                          : std::uint16_t{0};
        log.events.push_back(
            BattleEvent{tick, EventKind::Fired, shooter.id, after[best].id, hp[best]});
        if (hp[best] == 0) {
            alive[best] = false;
            log.events.push_back(
                BattleEvent{tick, EventKind::Killed, shooter.id, after[best].id, 0});
        }
    }
}

/// Appends accepted orders (Sim's command log grew) as Ordered events.
void record_orders(const Sim& sim, std::size_t& logged, BattleLog& log) {
    const std::vector<Command>& commands = sim.command_log();
    for (; logged < commands.size(); ++logged) {
        const Command& c = commands[logged];
        log.events.push_back(
            BattleEvent{c.tick, EventKind::Ordered, c.order.unit,
                        c.order.kind == OrderKind::Attack ? c.order.target : UnitId{},
                        static_cast<std::uint16_t>(c.order.kind)});
    }
}

void command_phase(Sim& sim, ai::ICommander* commander, Side side, std::vector<Order>& orders) {
    if (commander == nullptr) {
        return;
    }
    orders.clear();
    commander->think(ai::observe(sim, side), orders);
    for (const Order& order : orders) {
        sim.enqueue(order);
    }
}

void append_number(std::string& out, std::uint64_t value) { out += std::to_string(value); }

/// Q16.16 raw -> tiles with one decimal digit (truncated towards -infinity), integers only.
void append_tiles(std::string& out, std::int32_t raw) {
    std::int64_t tenths = (static_cast<std::int64_t>(raw) * 10) >> Fixed::kShift;
    if (tenths < 0) {
        out += '-';
        tenths = -tenths;
    }
    append_number(out, static_cast<std::uint64_t>(tenths / 10));
    out += '.';
    out += static_cast<char>('0' + tenths % 10);
}

void append_status_lines(std::string& out, Tick tick,
                         const std::map<std::uint32_t, UnitState>& units) {
    for (const auto& [id, s] : units) {
        if (stance_of(s.attrs) == Stance::Dead || s.hp == 0) {
            continue;
        }
        out += "tick ";
        append_number(out, tick);
        out += " status unit ";
        append_number(out, id);
        out += ' ';
        out += to_string(kind_of(s.attrs));
        out += " side ";
        out += to_string(side_of(s.attrs));
        out += " pos ";
        append_tiles(out, s.x);
        out += ' ';
        append_tiles(out, s.y);
        out += " hp ";
        append_number(out, s.hp);
        out += " stance ";
        out += to_string(stance_of(s.attrs));
        out += '\n';
    }
}

} // namespace

std::string BattleLog::to_text(Tick status_period) const {
    std::string out;
    std::map<std::uint32_t, UnitState> units;
    std::size_t next = 0;

    const auto apply_status = [&units](const BattleEvent& e) {
        UnitState& s = units[e.actor.value];
        if (e.kind == kStatusPos) {
            s.x = static_cast<std::int32_t>(e.subject.value);
            s.hp = e.subject_hp;
        } else {
            s.y = static_cast<std::int32_t>(e.subject.value);
            s.attrs = e.subject_hp;
        }
    };

    for (Tick t = 0; t <= ticks; ++t) {
        while (next < events.size() && events[next].tick <= t && is_status(events[next].kind)) {
            apply_status(events[next]);
            ++next;
        }
        if (t == 0 || (status_period != 0 && t % status_period == 0)) {
            append_status_lines(out, t, units);
        }
        while (next < events.size() && events[next].tick <= t) {
            const BattleEvent& e = events[next++];
            if (is_status(e.kind)) {
                apply_status(e);
                continue;
            }
            out += "tick ";
            append_number(out, e.tick);
            switch (e.kind) {
            case EventKind::Ordered:
                out += " ordered unit ";
                append_number(out, e.actor.value);
                out += ' ';
                out += to_string(static_cast<OrderKind>(e.subject_hp));
                break;
            case EventKind::Fired:
                out += " fired unit ";
                append_number(out, e.actor.value);
                out += " -> unit ";
                append_number(out, e.subject.value);
                out += " hp ";
                append_number(out, e.subject_hp);
                break;
            case EventKind::Killed:
                out += " killed unit ";
                append_number(out, e.subject.value);
                out += " by unit ";
                append_number(out, e.actor.value);
                break;
            }
            out += '\n';
        }
        if (t == std::numeric_limits<Tick>::max()) {
            break;
        }
    }

    std::uint64_t living = 0;
    for (const auto& [id, s] : units) {
        if (stance_of(s.attrs) != Stance::Dead && s.hp != 0) {
            ++living;
        }
    }
    out += "end tick ";
    append_number(out, ticks);
    out += " seed ";
    append_number(out, seed);
    out += " units ";
    append_number(out, living);
    out += " hash ";
    append_number(out, final_hash);
    out += '\n';
    return out;
}

BattleLog run_battle(const Scenario& scenario, std::span<const Command> script, Tick ticks,
                     ai::ICommander* player, ai::ICommander* enemy) {
    std::vector<Command> commands(script.begin(), script.end());
    std::stable_sort(commands.begin(), commands.end(),
                     [](const Command& a, const Command& b) { return a.tick < b.tick; });

    Sim sim = Sim::create(scenario);
    BattleLog log;
    log.ticks = ticks;
    log.seed = scenario.seed;

    std::vector<UnitState> last;
    std::vector<bool> seen;
    std::vector<Order> orders;
    std::size_t next_command = 0;
    std::size_t logged = 0;

    for (Tick t = 0; t < ticks; ++t) {
        record_status(sim, last, seen, log);
        while (next_command < commands.size() && commands[next_command].tick <= t) {
            sim.enqueue(commands[next_command].order);
            ++next_command;
        }
        command_phase(sim, player, Side::Player, orders);
        command_phase(sim, enemy, Side::Enemy, orders);
        record_orders(sim, logged, log);

        const std::vector<Unit> before = sim.units();
        sim.step();
        record_fire(before, sim, t, log);
    }
    record_status(sim, last, seen, log);
    log.final_hash = sim.state_hash();
    return log;
}

const char* to_string(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::Tank:
        return "tank";
    case UnitKind::Turret:
        return "turret";
    case UnitKind::Factory:
        return "factory";
    }
    return "unknown";
}

const char* to_string(Side side) noexcept {
    switch (side) {
    case Side::Player:
        return "player";
    case Side::Enemy:
        return "enemy";
    }
    return "unknown";
}

const char* to_string(Stance stance) noexcept {
    switch (stance) {
    case Stance::Idle:
        return "idle";
    case Stance::MoveTo:
        return "move";
    case Stance::GuardAt:
        return "guard";
    case Stance::Patrol:
        return "patrol";
    case Stance::Attack:
        return "attack";
    case Stance::Dead:
        return "dead";
    }
    return "unknown";
}

const char* to_string(OrderKind kind) noexcept {
    switch (kind) {
    case OrderKind::MoveTo:
        return "move";
    case OrderKind::GuardAt:
        return "guard";
    case OrderKind::Patrol:
        return "patrol";
    case OrderKind::Attack:
        return "attack";
    case OrderKind::Stop:
        return "stop";
    }
    return "unknown";
}

} // namespace protekt::sim
